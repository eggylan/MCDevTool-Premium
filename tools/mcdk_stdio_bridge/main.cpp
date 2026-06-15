#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#include "../mcdk/modules/mcp_tool_definitions.hpp"

#include <httplib.h>
#include <mcp_message.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <clocale>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

#ifdef _WIN32
    void configureStdioAndUtf8Console() {
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);
        std::setlocale(LC_ALL, ".UTF-8");
    }
#else
    void configureStdioAndUtf8Console() { std::setlocale(LC_ALL, ""); }
#endif

    using json = nlohmann::ordered_json;

    constexpr const char* BridgeName               = "Minecraft(BE) MCP Stdio Bridge(MCDK)";
    constexpr const char* BridgeVersion            = "0.1.0";
    constexpr const char* DefaultHost              = "localhost";
    constexpr int         DefaultPort              = 19133;
    constexpr const char* StreamableEndpoint       = "/mcp";
    constexpr int         ConnectTimeoutSeconds    = 1;
    constexpr int         ReadWriteTimeoutSeconds  = 30;
    constexpr int         InitializationTimeoutSec = 3;
    constexpr int         SseReadTimeoutSeconds    = 60; // 事件流读超时;显著长于 mcdk 1 秒心跳间隔

    struct BridgeConfig {
        std::string host = DefaultHost;
        int         port = DefaultPort;
    };

    std::string trim(std::string_view value) {
        auto begin = value.begin();
        auto end   = value.end();
        while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
            ++begin;
        }
        while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
            --end;
        }
        return std::string(begin, end);
    }

    bool parseInteger(const std::string& value, int& out) {
        try {
            size_t index = 0;
            int    port  = std::stoi(value, &index);
            if (index != value.size() || port <= 0 || port > 65535) {
                return false;
            }
            out = port;
            return true;
        } catch (...) {
            return false;
        }
    }

    BridgeConfig parseArgs(int argc, char** argv) {
        BridgeConfig config;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i] ? argv[i] : "";
            auto readNext = [&](std::string& target) {
                if (i + 1 < argc) {
                    target = argv[++i] ? argv[i] : "";
                }
            };

            if (arg == "--host" || arg == "-h") {
                readNext(config.host);
            } else if (arg.rfind("--host=", 0) == 0) {
                config.host = arg.substr(7);
            } else if (arg == "--port" || arg == "-p") {
                std::string portText;
                readNext(portText);
                int parsedPort = config.port;
                if (parseInteger(portText, parsedPort)) {
                    config.port = parsedPort;
                }
            } else if (arg.rfind("--port=", 0) == 0) {
                int parsedPort = config.port;
                if (parseInteger(arg.substr(7), parsedPort)) {
                    config.port = parsedPort;
                }
            } else {
                int parsedPort = config.port;
                if (parseInteger(arg, parsedPort)) {
                    config.port = parsedPort;
                }
            }
        }
        if (config.host.empty()) {
            config.host = DefaultHost;
        }
        return config;
    }

    json makeTextContent(const std::string& text) {
        return json::array({{{"type", "text"}, {"text", text}}});
    }

    json makeToolErrorResult(const std::string& text) {
        return json{{"isError", true}, {"content", makeTextContent(text)}};
    }

    json makeErrorResponse(const json& id, mcp::error_code code, const std::string& message) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", static_cast<int>(code)}, {"message", message}}}};
    }

    json makeSuccessResponse(const json& id, const json& result) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
    }

    class StdioTransport {
    public:
        std::optional<json> readMessage() {
            std::string firstLine;
            if (!std::getline(std::cin, firstLine)) {
                return std::nullopt;
            }
            if (!firstLine.empty() && firstLine.back() == '\r') {
                firstLine.pop_back();
            }
            if (firstLine.empty()) {
                return std::nullopt;
            }

            std::string headerKey = firstLine.substr(0, firstLine.find(':'));
            for (auto& c : headerKey) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }

            if (headerKey == "content-length") {
                auto colon = firstLine.find(':');
                if (colon == std::string::npos) {
                    return std::nullopt;
                }

                size_t contentLength = 0;
                try {
                    contentLength = static_cast<size_t>(std::stoull(trim(std::string_view(firstLine).substr(colon + 1))));
                } catch (...) {
                    return std::nullopt;
                }

                std::string headerLine;
                while (std::getline(std::cin, headerLine)) {
                    if (!headerLine.empty() && headerLine.back() == '\r') {
                        headerLine.pop_back();
                    }
                    if (headerLine.empty()) {
                        break;
                    }
                }

                std::string body(contentLength, '\0');
                std::cin.read(body.data(), static_cast<std::streamsize>(contentLength));
                if (std::cin.gcount() != static_cast<std::streamsize>(contentLength)) {
                    return std::nullopt;
                }
                return json::parse(body, nullptr, false);
            }

            return json::parse(firstLine, nullptr, false);
        }

        void writeMessage(const json& message) {
            std::lock_guard<std::mutex> lock(writeMutex_);
            std::cout << message.dump() << '\n';
            std::cout.flush();
        }

    private:
        std::mutex writeMutex_;
    };

    class GameMcpClient {
    public:
        explicit GameMcpClient(BridgeConfig config) : config_(std::move(config)) {}
        ~GameMcpClient() { stopNotificationStream(); }

        // 启动 SSE 通知转发:后台线程对 mcdk 开 GET 事件流,把服务端通知(如 tools/list_changed)
        // 经 sink 转发到 stdio 客户端。会话建立/重连时自动(重)开流。
        void startNotificationStream(std::function<void(const json&)> sink) {
            {
                std::lock_guard<std::mutex> lock(sseMtx_);
                if (sseRunning_.load()) {
                    return;
                }
                notificationSink_ = std::move(sink);
                sseRunning_.store(true);
            }
            sseThread_ = std::thread([this]() { sseLoop(); });
        }

        void stopNotificationStream() {
            if (!sseRunning_.exchange(false)) {
                return;
            }

            std::shared_ptr<httplib::Client> activeClient;
            {
                std::lock_guard<std::mutex> lock(sseMtx_);
                sseSessionId_.clear();
                ++sseGen_;
                activeClient = activeSseClient_;
            }
            sseCv_.notify_all();
            if (activeClient) {
                activeClient->stop();
            }
            if (sseThread_.joinable()) {
                sseThread_.join();
            }
            notificationSink_ = nullptr;
        }

        json callTool(const std::string& name, const json& arguments) {
            std::string error;
            if (!ensureConnected(error)) {
                return makeToolErrorResult(error);
            }

            json response;
            if (!postJson(json{{"jsonrpc", "2.0"}, {"id", nextId_++}, {"method", "tools/call"}, {"params", {{"name", name}, {"arguments", arguments}}}}, response, error)) {
                connected_ = false;
                sessionId_.clear();
                if (!ensureConnected(error)) {
                    return makeToolErrorResult(error);
                }
                if (!postJson(json{{"jsonrpc", "2.0"}, {"id", nextId_++}, {"method", "tools/call"}, {"params", {{"name", name}, {"arguments", arguments}}}}, response, error)) {
                    connected_ = false;
                    sessionId_.clear();
                    return makeToolErrorResult(error);
                }
            }

            if (response.contains("error")) {
                const auto& err = response["error"];
                return makeToolErrorResult("MCDK game MCP returned an error: " + err.value("message", response.dump()));
            }
            if (!response.contains("result")) {
                return makeToolErrorResult("MCDK game MCP returned an invalid response: " + response.dump());
            }
            return response["result"];
        }

        // 实时转发 tools/list 到 mcdk,返回 {"tools":[...]}（反映逐工具开关与动态自定义工具）。
        // 连接失败返回 null（由调用方决定是否回退到静态内置集）。
        json listTools(std::string& error) {
            if (!ensureConnected(error)) {
                return json();
            }
            json request = {{"jsonrpc", "2.0"}, {"id", nextId_++}, {"method", "tools/list"}, {"params", json::object()}};
            json response;
            if (!postJson(request, response, error)) {
                connected_ = false;
                sessionId_.clear();
                if (!ensureConnected(error)) {
                    return json();
                }
                request["id"] = nextId_++;
                if (!postJson(request, response, error)) {
                    connected_ = false;
                    sessionId_.clear();
                    return json();
                }
            }
            if (response.contains("error") || !response.contains("result")) {
                error = "MCDK game MCP returned an invalid tools/list response";
                return json();
            }
            return response["result"];
        }

    private:
        bool ensureConnected(std::string& error) {
            if (connected_ && ping()) {
                return true;
            }
            connected_ = false;
            sessionId_.clear();
            return initialize(error);
        }

        bool initialize(std::string& error) {
            httplib::Client client(baseUrl());
            configureClient(client, InitializationTimeoutSec);

            json initializeRequest = {
                {"jsonrpc", "2.0"},
                {"id", nextId_++},
                {"method", "initialize"},
                {"params",
                 {{"protocolVersion", mcp::MCP_VERSION},
                  {"capabilities", json::object()},
                  {"clientInfo", {{"name", "MCDKStdioBridge"}, {"version", BridgeVersion}}}}}
            };

            auto result = client.Post(StreamableEndpoint, initializeRequest.dump(), "application/json");
            if (!result) {
                error = notReadyMessage("cannot connect to the MCDK game MCP endpoint");
                return false;
            }
            if (result->status / 100 != 2) {
                error = notReadyMessage("MCDK game MCP returned HTTP " + std::to_string(result->status));
                return false;
            }

            json response = json::parse(result->body, nullptr, false);
            if (response.is_discarded() || response.contains("error")) {
                error = notReadyMessage("MCDK game MCP initialization failed");
                return false;
            }

            auto sessionHeader = result->headers.find("Mcp-Session-Id");
            if (sessionHeader == result->headers.end() || sessionHeader->second.empty()) {
                error = notReadyMessage("MCDK game MCP did not provide a session id");
                return false;
            }

            sessionId_ = sessionHeader->second;
            connected_ = true;

            json initializedNotification = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
            json ignored;
            std::string ignoredError;
            postJson(initializedNotification, ignored, ignoredError);

            // 通知 SSE 监听线程切换到新会话(打开/重开 GET 事件流,接收 tools/list_changed 等)。
            setSseSession(sessionId_);
            return true;
        }

        bool ping() {
            json        response;
            std::string error;
            bool ok = postJson(json{{"jsonrpc", "2.0"}, {"id", nextId_++}, {"method", "ping"}}, response, error);
            return ok && !response.contains("error");
        }

        bool postJson(const json& request, json& response, std::string& error) {
            httplib::Client client(baseUrl());
            configureClient(client, ReadWriteTimeoutSeconds);

            httplib::Headers headers;
            headers.emplace("Content-Type", "application/json");
            if (!sessionId_.empty()) {
                headers.emplace("Mcp-Session-Id", sessionId_);
            }

            auto result = client.Post(StreamableEndpoint, headers, request.dump(), "application/json");
            if (!result) {
                error = notReadyMessage("cannot connect to the MCDK game MCP endpoint");
                return false;
            }
            if (request.contains("id") && result->status / 100 != 2) {
                error = notReadyMessage("MCDK game MCP returned HTTP " + std::to_string(result->status));
                return false;
            }
            if (!request.contains("id")) {
                response = json::object();
                return true;
            }

            response = json::parse(result->body, nullptr, false);
            if (response.is_discarded()) {
                error = notReadyMessage("MCDK game MCP returned invalid JSON");
                return false;
            }
            return true;
        }

        void configureClient(httplib::Client& client, int readTimeoutSeconds) const {
            client.set_connection_timeout(ConnectTimeoutSeconds, 0);
            client.set_read_timeout(readTimeoutSeconds, 0);
            client.set_write_timeout(readTimeoutSeconds, 0);
        }

        std::string baseUrl() const { return "http://" + config_.host + ":" + std::to_string(config_.port); }

        std::string notReadyMessage(const std::string& detail) const {
            return "Minecraft has not been launched through MCDK, or MCDK MCP is not enabled/configured. "
                   "Please start the game with MCDK and enable mcp_server_config first. Target endpoint: "
                + baseUrl() + StreamableEndpoint + ". Detail: " + detail;
        }

        // 设置/切换 SSE 监听目标会话(initialize 成功后调用);bump 代次以让旧流自行中止。
        void setSseSession(const std::string& sid) {
            std::shared_ptr<httplib::Client> activeClient;
            {
                std::lock_guard<std::mutex> lk(sseMtx_);
                sseSessionId_ = sid;
                ++sseGen_;
                activeClient = activeSseClient_;
            }
            sseCv_.notify_all();
            if (activeClient) {
                activeClient->stop();
            }
        }

        // 后台线程:对当前会话开 GET 事件流,持续把服务端通知转发到 stdio。
        void sseLoop() {
            while (sseRunning_.load()) {
                std::string sid;
                uint64_t    gen = 0;
                {
                    std::unique_lock<std::mutex> lk(sseMtx_);
                    sseCv_.wait(lk, [&]() { return !sseRunning_.load() || !sseSessionId_.empty(); });
                    if (!sseRunning_.load()) {
                        break;
                    }
                    sid = sseSessionId_;
                    gen = sseGen_;
                }

                auto client = std::make_shared<httplib::Client>(baseUrl());
                client->set_connection_timeout(ConnectTimeoutSeconds, 0);
                client->set_read_timeout(SseReadTimeoutSeconds, 0);

                httplib::Headers headers = {
                    {  "Mcp-Session-Id", sid},
                    {"Accept", "text/event-stream"}
                };

                {
                    std::lock_guard<std::mutex> lk(sseMtx_);
                    if (!sseRunning_.load() || gen != sseGen_) {
                        continue;
                    }
                    activeSseClient_ = client;
                }

                std::string buf;
                int         responseStatus = 0;
                client->Get(
                    StreamableEndpoint,
                    headers,
                    [&](const httplib::Response& response) {
                        responseStatus = response.status;
                        return response.status / 100 == 2;
                    },
                    [&](const char* data, size_t len) -> bool {
                        if (!sseRunning_.load()) {
                            return false;
                        }
                        {
                            std::lock_guard<std::mutex> lk(sseMtx_);
                            if (gen != sseGen_) {
                                return false; // 会话已切换,放弃旧流
                            }
                        }
                        buf.append(data, len);
                        drainSseFrames(buf);
                        return true;
                    }
                );

                {
                    std::lock_guard<std::mutex> lk(sseMtx_);
                    if (activeSseClient_ == client) {
                        activeSseClient_.reset();
                    }
                }

                // A 404 is permanent for this session (typically mcdk restarted and lost its
                // in-memory sessions). Stop retrying the stale SSE target; the next tool request
                // will use the existing request path to initialize a fresh MCP session.
                if (responseStatus == 404) {
                    std::lock_guard<std::mutex> lk(sseMtx_);
                    if (gen == sseGen_ && sseSessionId_ == sid) {
                        sseSessionId_.clear();
                        ++sseGen_;
                    }
                    continue;
                }

                // 流结束(空闲超时/断开/会话切换)。若仍在运行且会话未变,稍后重开。
                if (sseRunning_.load()) {
                    std::unique_lock<std::mutex> lk(sseMtx_);
                    sseCv_.wait_for(lk, std::chrono::milliseconds(300), [&]() {
                        return !sseRunning_.load() || gen != sseGen_;
                    });
                }
            }
        }

        // 从 SSE 缓冲解析完整事件帧(以空行分隔),拼接 data: 行,JSON 解析后转发(仅含 method 的通知)。
        void drainSseFrames(std::string& buf) {
            for (;;) {
                size_t end   = buf.find("\n\n");
                size_t delim = 2;
                size_t endr  = buf.find("\r\n\r\n");
                if (endr != std::string::npos && (end == std::string::npos || endr < end)) {
                    end   = endr;
                    delim = 4;
                }
                if (end == std::string::npos) {
                    return;
                }
                std::string frame = buf.substr(0, end);
                buf.erase(0, end + delim);

                std::string        data;
                std::istringstream fs(frame);
                std::string        line;
                while (std::getline(fs, line)) {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    if (line.rfind("data:", 0) == 0) {
                        std::string v = line.substr(5);
                        if (!v.empty() && v.front() == ' ') {
                            v.erase(0, 1);
                        }
                        if (!data.empty()) {
                            data += "\n";
                        }
                        data += v;
                    }
                }
                if (data.empty()) {
                    continue;
                }
                json msg = json::parse(data, nullptr, false);
                if (msg.is_discarded() || !msg.is_object() || msg.value("jsonrpc", "") != "2.0"
                    || !msg.contains("method") || (msg.contains("id") && !msg["id"].is_null())) {
                    continue; // bridge 不代理服务端请求的响应链,仅转发 JSON-RPC 通知
                }
                if (notificationSink_) {
                    notificationSink_(msg);
                }
            }
        }

        BridgeConfig config_;
        std::string  sessionId_;
        bool         connected_ = false;
        int          nextId_    = 1;

        // SSE 通知转发
        std::function<void(const json&)> notificationSink_;
        std::atomic<bool>                sseRunning_{false};
        std::thread                      sseThread_;
        std::mutex                       sseMtx_;
        std::condition_variable          sseCv_;
        std::string                      sseSessionId_;
        uint64_t                         sseGen_ = 0;
        std::shared_ptr<httplib::Client> activeSseClient_;
    };

    class BridgeServer {
    public:
        explicit BridgeServer(BridgeConfig config) : gameClient_(std::move(config)) {}

        void run() {
            StdioTransport transport;
            // 启动 SSE 通知转发:把 mcdk 推送的 tools/list_changed 等通知透传到 stdio 客户端,
            // 使运行时工具增删无需 /mcp 重连即可自动刷新。
            gameClient_.startNotificationStream([&transport](const json& message) {
                transport.writeMessage(message);
            });
            while (true) {
                auto message = transport.readMessage();
                if (!message.has_value()) {
                    break;
                }
                if (message->is_discarded()) {
                    continue;
                }
                auto response = handleMessage(*message);
                if (response.has_value()) {
                    transport.writeMessage(*response);
                }
            }
            gameClient_.stopNotificationStream(); // 在 transport 析构前停止 SSE 线程
        }

    private:
        std::optional<json> handleMessage(const json& message) {
            if (!message.is_object() || !message.contains("method")) {
                json id = message.is_object() && message.contains("id") ? message["id"] : nullptr;
                return makeErrorResponse(id, mcp::error_code::invalid_request, "Invalid JSON-RPC request");
            }

            const bool isNotification = !message.contains("id") || message["id"].is_null();
            json       id             = isNotification ? nullptr : message["id"];
            std::string method         = message.value("method", "");
            json        params         = message.value("params", json::object());

            if (isNotification) {
                return std::nullopt;
            }

            if (method == "initialize") {
                return makeSuccessResponse(
                    id,
                    json{
                        {"protocolVersion", mcp::MCP_VERSION},
                        {"capabilities", {{"tools", {{"listChanged", true}}}}},
                        {"serverInfo", {{"name", BridgeName}, {"version", BridgeVersion}}}
                    }
                );
            }
            if (method == "ping") {
                return makeSuccessResponse(id, json::object());
            }
            if (method == "tools/list") {
                // 实时转发 mcdk 的工具列表,反映 .mcdev.json 逐工具开关与动态自定义工具。
                std::string error;
                json        result = gameClient_.listTools(error);
                if (result.is_object() && result.contains("tools")) {
                    return makeSuccessResponse(id, result);
                }
                // 回退:mcdk 不可达时返回静态内置工具集(降级,可能含被禁用项 / 不含自定义工具)。
                json tools = json::array();
                for (const auto& tool : mcdk::mcp_tool_definitions::buildAllTools()) {
                    tools.push_back(tool.to_json());
                }
                return makeSuccessResponse(id, json{{"tools", tools}});
            }
            if (method == "tools/call") {
                if (!params.is_object() || !params.contains("name")) {
                    return makeErrorResponse(id, mcp::error_code::invalid_params, "Missing 'name' parameter");
                }
                std::string toolName  = params.value("name", "");
                json        arguments = params.value("arguments", json::object());
                return makeSuccessResponse(id, gameClient_.callTool(toolName, arguments));
            }
            if (method == "resources/list") {
                return makeSuccessResponse(id, json{{"resources", json::array()}});
            }
            if (method == "resources/templates/list") {
                return makeSuccessResponse(id, json{{"resourceTemplates", json::array()}});
            }
            if (method == "prompts/list") {
                return makeSuccessResponse(id, json{{"prompts", json::array()}});
            }

            return makeErrorResponse(id, mcp::error_code::method_not_found, "Method not found: " + method);
        }

        GameMcpClient gameClient_;
    };

} // namespace

int main(int argc, char** argv) {
    configureStdioAndUtf8Console();
    BridgeServer server(parseArgs(argc, argv));
    server.run();
    return 0;
}
