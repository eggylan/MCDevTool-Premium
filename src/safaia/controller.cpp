#include "mcdevtool/safaia/controller.h"

#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace MCDevTool::Safaia {

    void SafaiaController::configure(const SafaiaControllerConfig& cfg) { cfg_ = cfg; }

    void SafaiaController::log(const std::string& level, const std::string& msg) {
        if (logFn_) {
            logFn_(level, msg);
        }
    }

#ifdef _WIN32

    SafaiaController::~SafaiaController() { stop(); }

    bool SafaiaController::startServer() {
        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) {
            log("error", "Safaia: socket() failed");
            return false;
        }

        BOOL reuse = TRUE;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<u_short>(cfg_.bindPort));
        if (cfg_.bindIp.empty() || cfg_.bindIp == "0.0.0.0") {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (inet_pton(AF_INET, cfg_.bindIp.c_str(), &addr.sin_addr) != 1) {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        }

        if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            closesocket(listenSock);
            log("error", "Safaia: bind() failed");
            return false;
        }

        int addrLen = sizeof(addr);
        getsockname(listenSock, reinterpret_cast<sockaddr*>(&addr), &addrLen);
        boundPort_ = ntohs(addr.sin_port);

        if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(listenSock);
            log("error", "Safaia: listen() failed");
            return false;
        }

        u_long nonBlocking = 1;
        ioctlsocket(listenSock, FIONBIO, &nonBlocking);

        listenSock_ = reinterpret_cast<void*>(listenSock);
        return true;
    }

    bool SafaiaController::start() {
        if (!cfg_.enabled || running_.load()) {
            return false;
        }
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            log("error", "Safaia: WSAStartup failed");
            return false;
        }
        if (!startServer()) {
            WSACleanup();
            return false;
        }

        // 发现目标：配置覆盖或自动枚举本机 IPv4。
        std::vector<std::string> targets = cfg_.targetIps;
        if (targets.empty()) {
            targets = enumerateLocalIPv4();
        }
        discovery_.configure(cfg_.advertiseIp, boundPort_, targets, cfg_.discoveryIntervalMs);
        discovery_.start();

        running_.store(true);
        acceptThread_.emplace([this]() { acceptLoop(); });

        log("info",
            "Safaia controller started on " + cfg_.bindIp + ":" + std::to_string(boundPort_)
                + " (advertise " + cfg_.advertiseIp + ", discovery -> 26613..26622)");
        return true;
    }

    void SafaiaController::acceptLoop() {
        SOCKET listenSock = reinterpret_cast<SOCKET>(listenSock_);
        while (running_.load() && listenSock != INVALID_SOCKET) {
            sockaddr_in cliAddr{};
            int         cliLen = sizeof(cliAddr);
            SOCKET      cli    = accept(listenSock, reinterpret_cast<sockaddr*>(&cliAddr), &cliLen);
            if (cli == INVALID_SOCKET) {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                break; // socket 已关闭或致命错误
            }
            char ipbuf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &cliAddr.sin_addr, ipbuf, sizeof(ipbuf));
            log("info", std::string("Safaia: game connected from ") + ipbuf);
            handleClient(reinterpret_cast<void*>(cli));
            log("info", "Safaia: game disconnected; waiting for reconnect");
        }
    }

    void SafaiaController::handleClient(void* clientSock) {
        SOCKET sock = reinterpret_cast<SOCKET>(clientSock);

        // 设置接收超时，便于周期检查 running_。
        DWORD timeoutMs = 200;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        {
            std::lock_guard<std::mutex> lk(clientMtx_);
            clientSock_ = clientSock;
        }
        framer_.reset();
        drainResponses();
        enableSpawned_.store(false);
        clientConnected_.store(true);
        state_.setConnected(true);

        std::vector<uint8_t> buf(8192);
        while (running_.load()) {
            int n = recv(sock, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
            if (n > 0) {
                auto frames = framer_.input(buf.data(), static_cast<size_t>(n));
                for (const auto& f : frames) {
                    onFrame(f);
                }
            } else if (n == 0) {
                break; // 对端正常关闭
            } else {
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
                    continue;
                }
                break; // 连接错误
            }
        }

        // ── 断连清理 ──
        clientConnected_.store(false);
        state_.resetForDisconnect();
        {
            std::lock_guard<std::mutex> lk(clientMtx_);
            clientSock_ = nullptr;
        }
        {
            // 唤醒可能在等待响应的 RPC（callRpc 谓词会看到 clientConnected_=false）。
            std::lock_guard<std::mutex> lk(respMtx_);
            respCv_.notify_all();
        }
        // enable 线程此时 callRpc 会快速返回；安全 join。
        if (enableThread_.has_value()) {
            if (enableThread_->joinable()) {
                enableThread_->join();
            }
            enableThread_.reset();
        }
        closesocket(sock);
    }

    bool SafaiaController::sendFrame(int32_t protocolId, const nlohmann::json& payload) {
        SOCKET sock;
        {
            std::lock_guard<std::mutex> lk(clientMtx_);
            if (!clientSock_) {
                return false;
            }
            sock = reinterpret_cast<SOCKET>(clientSock_);
        }
        std::vector<uint8_t> bytes = ProtocolFramer::packJson(protocolId, payload);

        std::lock_guard<std::mutex> lk(sendMtx_);
        size_t                      sent = 0;
        while (sent < bytes.size()) {
            int n = send(
                sock,
                reinterpret_cast<const char*>(bytes.data() + sent),
                static_cast<int>(bytes.size() - sent),
                0
            );
            if (n == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    void SafaiaController::stop() {
        if (!running_.exchange(false)) {
            // 未运行，仍尝试清理发现/句柄。
        }
        discovery_.stop();

        // 关闭 client socket 以解阻 recv。
        {
            std::lock_guard<std::mutex> lk(clientMtx_);
            if (clientSock_) {
                closesocket(reinterpret_cast<SOCKET>(clientSock_));
                clientSock_ = nullptr;
            }
        }
        clientConnected_.store(false);
        {
            std::lock_guard<std::mutex> lk(respMtx_);
            respCv_.notify_all();
        }
        // 关闭 listen socket 以解阻 accept。
        if (listenSock_) {
            closesocket(reinterpret_cast<SOCKET>(listenSock_));
            listenSock_ = nullptr;
        }
        if (acceptThread_.has_value()) {
            if (acceptThread_->joinable()) {
                acceptThread_->join();
            }
            acceptThread_.reset();
        }
        if (enableThread_.has_value()) {
            if (enableThread_->joinable()) {
                enableThread_->join();
            }
            enableThread_.reset();
        }
        WSACleanup();
    }

#else // ── 非 Windows 桩 ──

    SafaiaController::~SafaiaController() {}
    bool SafaiaController::startServer() { return false; }
    bool SafaiaController::start() { return false; }
    void SafaiaController::acceptLoop() {}
    void SafaiaController::handleClient(void*) {}
    bool SafaiaController::sendFrame(int32_t, const nlohmann::json&) { return false; }
    void SafaiaController::stop() {}

#endif

    // ── 平台无关逻辑 ───────────────────────────────────────────────

    void SafaiaController::onFrame(const Frame& f) {
        switch (f.protocolId) {
            case MCProtocol::config: {
                auto cfg = nlohmann::json::parse(f.payload, nullptr, false);
                onConfig(cfg.is_discarded() ? nlohmann::json::object() : cfg);
                break;
            }
            case MCProtocol::heart:
                // 游戏侧心跳；接收即视为存活，无需回复（与 Python 探针一致）。
                break;
            case MCProtocol::message:
                // 游戏日志消息；P0 暂不消费。
                break;
            case MCProtocol::cmd:
                onCmdFrame(f.payload);
                break;
            case MCProtocol::leave:
                clientConnected_.store(false);
                break;
            default:
                break;
        }
    }

    void SafaiaController::onConfig(const nlohmann::json& cfg) {
        std::string name = cfg.is_object() ? cfg.value("name", std::string{}) : std::string{};
        log("info", "Safaia: config handshake from game" + (name.empty() ? std::string{} : (" (" + name + ")")));

        // 回复 connect_success（48）。
        sendFrame(MCProtocol::connect_success, nlohmann::json{{"notify", "pass"}});
        state_.setConnected(true);
        clientConnected_.store(true);

        // 首个 config 触发 enable 序列（独立线程，避免阻塞读循环）。
        if (!enableSpawned_.exchange(true)) {
            if (enableThread_.has_value()) {
                // 理论上上一连接的 enable 线程已在断连清理时 join；防御性 detach。
                if (enableThread_->joinable()) {
                    enableThread_->detach();
                }
                enableThread_.reset();
            }
            enableThread_.emplace([this]() { runEnableSequence(); });
        }
    }

    void SafaiaController::onCmdFrame(const std::string& payload) {
        auto inner = parseUidebugerResponse(payload);
        if (!inner.has_value() || !inner->is_object()) {
            return; // 非 uidebuger_call 或解析失败
        }
        int handle = inner->value("handle", -1);

        if (isOutOfBandHandle(handle)) {
            UiEvent ev;
            ev.handle     = handle;
            ev.handleName = handlerName(handle);
            ev.success    = inner->value("success", false);
            ev.data       = inner->contains("data") ? (*inner)["data"] : nlohmann::json();
            ev.seq        = state_.nextEventSeq();

            if (handle == RPCHandles::ScreenChanged) {
                // data = 裸字符串新顶层屏名（或对象含 name）。失效树缓存。
                std::string screen;
                if (ev.data.is_string()) {
                    screen = ev.data.get<std::string>();
                } else if (ev.data.is_object()) {
                    screen = ev.data.value("name", std::string{});
                }
                state_.invalidateTrees();
                if (!screen.empty()) {
                    state_.setCurrentScreen(screen);
                }
            } else { // ControlSelectionChanged
                state_.updateSelection(ev.data); // 内部去重 + 唤醒等待者
            }
            state_.appendEvent(ev);
            return;
        }

        // 普通响应（handle 0/1/2/3/5/6）→ 投递给在途请求。
        deliverResponse(*inner);
    }

    void SafaiaController::deliverResponse(const nlohmann::json& inner) {
        std::lock_guard<std::mutex> lk(respMtx_);
        respQueue_.push(inner);
        respCv_.notify_all();
    }

    void SafaiaController::drainResponses() {
        std::lock_guard<std::mutex> lk(respMtx_);
        std::queue<nlohmann::json>  empty;
        std::swap(respQueue_, empty);
    }

    bool SafaiaController::sendUidebuger(const std::string& func, const nlohmann::json& args) {
        return sendFrame(MCProtocol::cmd, buildUidebugerFrame(buildUidebugerInner(func, args)));
    }

    RpcResult SafaiaController::callRpc(const std::string& func, const nlohmann::json& args) {
        RpcResult r;
        if (!clientConnected_.load()) {
            r.error = "game not connected";
            return r;
        }

        std::lock_guard<std::mutex> serial(rpcMtx_); // 一次一个在途请求

        drainResponses();
        if (!sendUidebuger(func, args)) {
            r.error = "send failed (game not connected)";
            return r;
        }

        std::unique_lock<std::mutex> lk(respMtx_);
        bool got = respCv_.wait_for(lk, std::chrono::milliseconds(cfg_.rpcTimeoutMs), [&] {
            return !respQueue_.empty() || !running_.load() || !clientConnected_.load();
        });
        if (!respQueue_.empty()) {
            nlohmann::json inner = respQueue_.front();
            respQueue_.pop();
            lk.unlock();
            r.ok      = true;
            r.handle  = inner.value("handle", -1);
            r.success = inner.value("success", false);
            r.data    = inner.contains("data") ? inner["data"] : nlohmann::json();
            return r;
        }
        // 无响应：超时或断连
        if (!got || !clientConnected_.load()) {
            r.timeout = true;
            r.error   = clientConnected_.load() ? "rpc timeout" : "game disconnected during rpc";
        }
        return r;
    }

    RpcResult SafaiaController::getControlTree(const std::string& rootPath) {
        RpcResult r = callRpc("GetControlTree", nlohmann::json::array({rootPath}));
        if (r.ok && r.handle == RPCHandles::GetControlTree && r.success) {
            state_.cacheTree(rootPath, r.data);
            if (rootPath == "/") {
                // 树形：{data:{name,..}, path} 或直接 {name,..}
                std::string screen;
                if (r.data.is_object()) {
                    if (r.data.contains("data") && r.data["data"].is_object()) {
                        screen = r.data["data"].value("name", std::string{});
                    }
                    if (screen.empty()) {
                        screen = r.data.value("name", std::string{});
                    }
                }
                if (!screen.empty()) {
                    state_.setCurrentScreen(screen);
                }
            }
        }
        return r;
    }

    RpcResult SafaiaController::getControlsData(const std::vector<std::string>& paths) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& p : paths) {
            arr.push_back(p);
        }
        // GetControlsData(controlPaths) → args = [ [paths...] ]
        return callRpc("GetControlsData", nlohmann::json::array({arr}));
    }

    bool SafaiaController::setSelectedControls(const std::vector<std::string>& paths) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& p : paths) {
            arr.push_back(p);
        }
        return sendUidebuger("SetSelectedControls", nlohmann::json::array({arr}));
    }

    bool SafaiaController::setBoundsVisible(bool visible) {
        return sendUidebuger("SetBoundsVisible", nlohmann::json::array({visible}));
    }

    bool SafaiaController::setControlVisible(const std::string& path, bool visible) {
        return sendUidebuger("SetControlVisible", nlohmann::json::array({path, visible}));
    }

    bool SafaiaController::setEnabled(bool enabled) {
        return sendUidebuger("SetEnabled", nlohmann::json::array({enabled}));
    }

    void SafaiaController::runEnableSequence() {
        // SetEnabled 无 ack：fire-and-forget，随后用 GetControlTree(handle=5) 作就绪信号。
        setEnabled(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));

        for (int attempt = 1; attempt <= cfg_.enableRetries; ++attempt) {
            if (!clientConnected_.load() || !running_.load()) {
                return;
            }
            RpcResult r = getControlTree("/");
            if (r.ok && r.handle == RPCHandles::GetControlTree && r.success) {
                state_.setReady(true);
                log("info", "Safaia: UI debugger ready (screen=" + state_.currentScreen() + ")");
                return;
            }
            if (r.ok && r.handle == RPCHandles::NotEnabled) {
                log("warn", "Safaia: NotEnabled, re-sending SetEnabled");
                setEnabled(true);
            } else {
                log("info",
                    "Safaia: enable attempt " + std::to_string(attempt) + " -> "
                        + (r.timeout ? std::string("timeout") : r.handleName()));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.enableRetryIntervalMs));
        }
        log("warn", "Safaia: enable sequence exhausted; controller will keep listening for events");
    }

} // namespace MCDevTool::Safaia
