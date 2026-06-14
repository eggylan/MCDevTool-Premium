import json
import os
import queue
import subprocess
import sys
import threading
import time


def send_message(process, message):
    process.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
    process.stdin.flush()


def read_messages(stream, output_queue):
    for line in iter(stream.readline, ""):
        line = line.strip()
        if line:
            output_queue.put(json.loads(line))


def read_message(process, output_queue, timeout):
    try:
        return output_queue.get(timeout=timeout)
    except queue.Empty:
        if process.poll() is not None:
            raise RuntimeError("bridge exited with code %s" % process.returncode)
        raise RuntimeError("timed out waiting for bridge output")


def wait_for_id(process, output_queue, expected_id, timeout):
    deadline = time.monotonic() + timeout
    seen = []
    while time.monotonic() < deadline:
        message = read_message(process, output_queue, max(0.1, deadline - time.monotonic()))
        seen.append(message)
        if message.get("id") == expected_id:
            return message
    raise AssertionError("timed out waiting for id %s; received %r" % (expected_id, seen))


def list_tools(process, output_queue, request_id):
    send_message(process, {"jsonrpc": "2.0", "id": request_id, "method": "tools/list", "params": {}})
    response = wait_for_id(process, output_queue, request_id, 10)
    return {tool["name"] for tool in response["result"]["tools"]}


def wait_for_game_tools(process, output_queue, next_id):
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        names = list_tools(process, output_queue, next_id)
        next_id += 1
        if "health_check" in names and "resync_custom_tools" in names:
            return names, next_id
        time.sleep(2)
    raise AssertionError("game IPC did not expose health_check within 120 seconds")


def drain_messages(output_queue):
    while True:
        try:
            output_queue.get_nowait()
        except queue.Empty:
            break


def call_resync_and_wait_for_notification(process, output_queue, request_id):
    send_message(
        process,
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "tools/call",
            "params": {"name": "resync_custom_tools", "arguments": {}},
        },
    )
    response = None
    notification = None
    deadline = time.monotonic() + 15
    seen = []
    while time.monotonic() < deadline and (response is None or notification is None):
        message = read_message(process, output_queue, max(0.1, deadline - time.monotonic()))
        seen.append(message)
        if message.get("id") == request_id:
            response = message
        elif message.get("method") == "notifications/tools/list_changed":
            notification = message
    if response is None or notification is None:
        raise AssertionError(
            "resync did not produce both response and tools/list_changed; received %r" % seen
        )
    return response


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: test_stdio_bridge_sse_live.py <mcdk_stdio_bridge.exe>")

    bridge_path = os.path.abspath(sys.argv[1])
    global_tools_dir = os.path.join(os.environ["USERPROFILE"], ".mcdk", "mcp_tools")
    os.makedirs(global_tools_dir, exist_ok=True)

    tool_name = "codex_sse_probe_%s" % os.getpid()
    tool_path = os.path.join(global_tools_dir, tool_name + ".py")
    tool_source = """# -*- coding: utf-8 -*-
@mcp_tool(
    name=%r,
    description="Temporary tools/list_changed live probe.",
    params=[],
    side="server",
)
def codex_sse_probe():
    return {"ok": True}
""" % tool_name

    process = subprocess.Popen(
        [bridge_path, "--host", "127.0.0.1", "--port", "19133"],
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

    next_id = 1
    try:
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": next_id,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-03-26",
                    "capabilities": {},
                    "clientInfo": {"name": "live-sse-test", "version": "1.0"},
                },
            },
        )
        initialize = wait_for_id(process, output_queue, next_id, 5)
        assert initialize["result"]["capabilities"]["tools"]["listChanged"] is True
        next_id += 1

        names, next_id = wait_for_game_tools(process, output_queue, next_id)
        assert tool_name not in names

        # Cross the server's one-second event wait interval so the real cpp-mcp heartbeat path is exercised.
        time.sleep(2)
        drain_messages(output_queue)
        with open(tool_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(tool_source)

        call_resync_and_wait_for_notification(process, output_queue, next_id)
        next_id += 1
        names = list_tools(process, output_queue, next_id)
        next_id += 1
        assert tool_name in names, "probe tool was not registered after resync"

        os.remove(tool_path)
        drain_messages(output_queue)
        call_resync_and_wait_for_notification(process, output_queue, next_id)
        next_id += 1
        names = list_tools(process, output_queue, next_id)
        assert tool_name not in names, "probe tool remained registered after deletion"

        shutdown_started = time.monotonic()
        process.stdin.close()
        process.wait(timeout=20)
        shutdown_elapsed = time.monotonic() - shutdown_started
        assert process.returncode == 0
        assert shutdown_elapsed < 3, "bridge shutdown took %.2fs" % shutdown_elapsed
        print(
            "PASS: live add/remove each emitted tools/list_changed without bridge reconnect; "
            "shutdown %.2fs" % shutdown_elapsed
        )
    finally:
        if os.path.exists(tool_path):
            os.remove(tool_path)
        if process.poll() is None:
            process.kill()
            process.wait(timeout=3)
        stderr = process.stderr.read()
        if process.returncode not in (0, None) and stderr:
            sys.stderr.write(stderr)


if __name__ == "__main__":
    main()
