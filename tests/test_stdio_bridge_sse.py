import json
import os
import queue
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


SESSION_ID_PREFIX = "stdio-bridge-sse-test"


class FakeMcpState(object):
    def __init__(self):
        self.lock = threading.Lock()
        self.sse_connected = threading.Event()
        self.sse_reconnected = threading.Event()
        self.send_notification = threading.Event()
        self.drop_sse = threading.Event()
        self.stop = threading.Event()
        self.session_id = None
        self.initialize_count = 0
        self.sse_connection_count = 0
        self.rejected_get_count = 0

    def create_session(self):
        with self.lock:
            self.initialize_count += 1
            self.session_id = "%s-%d" % (SESSION_ID_PREFIX, self.initialize_count)
            return self.session_id

    def current_session(self):
        with self.lock:
            return self.session_id

    def invalidate_session(self):
        with self.lock:
            self.session_id = None
        self.drop_sse.set()

    def record_sse_connection(self):
        with self.lock:
            self.sse_connection_count += 1
            count = self.sse_connection_count
        self.sse_connected.set()
        if count >= 2:
            self.sse_reconnected.set()

    def record_rejected_get(self):
        with self.lock:
            self.rejected_get_count += 1

    def rejected_gets(self):
        with self.lock:
            return self.rejected_get_count


class FakeMcpHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    state = None

    def log_message(self, _format, *_args):
        return

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        request = json.loads(body.decode("utf-8"))
        method = request.get("method")

        if method == "initialize":
            session_id = self.state.create_session()
            result = {
                "jsonrpc": "2.0",
                "id": request["id"],
                "result": {
                    "protocolVersion": "2025-03-26",
                    "capabilities": {"tools": {"listChanged": True}},
                    "serverInfo": {"name": "fake-mcdk", "version": "1.0"},
                },
            }
            self._send_json(200, result, {"Mcp-Session-Id": session_id})
            return

        if method == "ping":
            self._send_json(200, {"jsonrpc": "2.0", "id": request["id"], "result": {}})
            return

        if self.headers.get("Mcp-Session-Id") != self.state.current_session():
            self._send_json(404, {"error": "Session not found"})
            return

        if method == "notifications/initialized":
            self._send_empty(202)
        elif method == "tools/list":
            self._send_json(
                200,
                {
                    "jsonrpc": "2.0",
                    "id": request["id"],
                    "result": {
                        "tools": [
                            {
                                "name": "fake_tool",
                                "description": "Integration test tool",
                                "inputSchema": {"type": "object", "properties": {}},
                            }
                        ]
                    },
                },
            )
        else:
            self._send_json(
                200,
                {
                    "jsonrpc": "2.0",
                    "id": request.get("id"),
                    "error": {"code": -32601, "message": "Method not found"},
                },
            )

    def do_GET(self):
        if self.headers.get("Mcp-Session-Id") != self.state.current_session():
            self.state.record_rejected_get()
            self._send_json(404, {"error": "Session not found"})
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        self.state.record_sse_connection()

        try:
            self._write_chunk(b": connected\r\n\r\n")
            if self.state.send_notification.wait(5):
                payload = (
                    b"event: message\r\n"
                    b"data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}\r\n\r\n"
                )
                self._write_chunk(payload)

            while not self.state.stop.wait(0.1):
                if self.state.drop_sse.is_set():
                    self.state.drop_sse.clear()
                    return
                self._write_chunk(b": keepalive\r\n\r\n")
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def _send_json(self, status, payload, extra_headers=None):
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if extra_headers:
            for key, value in extra_headers.items():
                self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def _send_empty(self, status):
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _write_chunk(self, payload):
        self.wfile.write(("%x\r\n" % len(payload)).encode("ascii"))
        self.wfile.write(payload)
        self.wfile.write(b"\r\n")
        self.wfile.flush()


def read_messages(stream, output_queue):
    for line in iter(stream.readline, ""):
        line = line.strip()
        if line:
            output_queue.put(json.loads(line))


def wait_for_message(output_queue, predicate, timeout, description):
    deadline = time.monotonic() + timeout
    seen = []
    while time.monotonic() < deadline:
        try:
            message = output_queue.get(timeout=max(0.01, deadline - time.monotonic()))
        except queue.Empty:
            break
        seen.append(message)
        if predicate(message):
            return message
    raise AssertionError("Timed out waiting for %s; received: %r" % (description, seen))


def wait_for_condition(predicate, timeout, description):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("Timed out waiting for %s" % description)


def send_message(process, message):
    process.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
    process.stdin.flush()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: test_stdio_bridge_sse.py <mcdk_stdio_bridge.exe>")

    bridge_path = os.path.abspath(sys.argv[1])
    if not os.path.isfile(bridge_path):
        raise SystemExit("Bridge executable not found: %s" % bridge_path)

    state = FakeMcpState()
    FakeMcpHandler.state = state
    server = ThreadingHTTPServer(("127.0.0.1", 0), FakeMcpHandler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    process = subprocess.Popen(
        [bridge_path, "--host", "127.0.0.1", "--port", str(server.server_port)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )
    output_queue = queue.Queue()
    reader = threading.Thread(target=read_messages, args=(process.stdout, output_queue), daemon=True)
    reader.start()

    try:
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-03-26",
                    "capabilities": {},
                    "clientInfo": {"name": "integration-test", "version": "1.0"},
                },
            },
        )
        initialize = wait_for_message(
            output_queue,
            lambda message: message.get("id") == 1,
            3,
            "bridge initialize response",
        )
        assert initialize["result"]["capabilities"]["tools"]["listChanged"] is True

        send_message(
            process,
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
        )
        tools_list = wait_for_message(
            output_queue,
            lambda message: message.get("id") == 2,
            5,
            "proxied tools/list response",
        )
        assert [tool["name"] for tool in tools_list["result"]["tools"]] == ["fake_tool"]
        assert state.sse_connected.wait(5), "bridge did not open the Streamable HTTP GET event stream"

        state.send_notification.set()
        notification = wait_for_message(
            output_queue,
            lambda message: message.get("method") == "notifications/tools/list_changed",
            5,
            "forwarded tools/list_changed notification",
        )
        assert "id" not in notification

        state.invalidate_session()
        wait_for_condition(lambda: state.rejected_gets() >= 1, 3, "one rejected stale-session GET")
        time.sleep(1)
        assert state.rejected_gets() == 1, (
            "bridge kept retrying a stale SSE session after HTTP 404; rejected GET count: %d"
            % state.rejected_gets()
        )

        send_message(
            process,
            {"jsonrpc": "2.0", "id": 3, "method": "tools/list", "params": {}},
        )
        reconnected_tools = wait_for_message(
            output_queue,
            lambda message: message.get("id") == 3,
            5,
            "tools/list response after stale SSE session",
        )
        assert [tool["name"] for tool in reconnected_tools["result"]["tools"]] == ["fake_tool"]
        assert state.sse_reconnected.wait(5), "bridge did not open a new SSE stream after reinitializing"
        assert state.initialize_count == 2, "expected exactly one MCP reinitialization"

        started = time.monotonic()
        process.stdin.close()
        process.wait(timeout=3)
        elapsed = time.monotonic() - started
        assert process.returncode == 0, "bridge exited with code %s" % process.returncode
        assert elapsed < 3, "bridge shutdown took %.2fs" % elapsed
        print(
            "PASS: tools/list_changed forwarded, stale SSE session retired after one 404, "
            "reinitialized successfully, and bridge stopped in %.2fs" % elapsed
        )
    finally:
        state.stop.set()
        server.shutdown()
        server.server_close()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=3)
        stderr = process.stderr.read()
        if process.returncode not in (0, None) and stderr:
            sys.stderr.write(stderr)


if __name__ == "__main__":
    main()
