#pragma once
// mcdk 内嵌 C++ Safaia 控制器：以 Safaia server 身份接管游戏 UI Debugger。
// 角色：TCP server（游戏回连）+ UDP 发现 + 握手 + 心跳接收 + 按 handle 配对的
// uidebuger_call 请求/响应路由 + 带外 4/7 事件分发 + 自动 enable 序列。
// 对照 temp/safaia_live_probe/{server,ui_probe,event_probe}.py。
// Winsock 实现仅 Windows；其余平台为桩（不会在非 Windows 路径被使用）。
#include "mcdevtool/safaia/protocol.h"
#include "mcdevtool/safaia/ui_debug_state.h"
#include "mcdevtool/safaia/discovery.h"

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <queue>
#include <functional>
#include <nlohmann/json.hpp>

namespace MCDevTool::Safaia {

    struct SafaiaControllerConfig {
        bool                     enabled              = false;
        std::string              bindIp               = "0.0.0.0";   // TCP server 绑定地址
        int                      bindPort             = 0;           // 0 = 系统自动分配
        std::string              advertiseIp          = "127.0.0.1"; // 写入 discovery payload（游戏回连地址）
        std::vector<std::string> targetIps;                          // UDP 发现目标；空 = 自动枚举本机 IPv4
        int                      discoveryIntervalMs  = 500;
        int                      rpcTimeoutMs         = 5000;
        int                      enableRetries        = 6;
        int                      enableRetryIntervalMs = 1500;
    };

    // 一次 uidebuger_call RPC 的结果。
    struct RpcResult {
        bool           ok      = false; // 收到有效响应（非超时、已连接）
        bool           timeout = false;
        int            handle  = -1;
        bool           success = false; // 内层 success
        nlohmann::json data;            // 内层 data
        std::string    error;

        std::string handleName() const { return MCDevTool::Safaia::handlerName(handle); }
    };

    // 日志回调（可选）：level 为 "info"/"warn"/"error"。
    using SafaiaLogFn = std::function<void(const std::string& level, const std::string& msg)>;

    class SafaiaController {
    public:
        SafaiaController() = default;
        ~SafaiaController();

        SafaiaController(const SafaiaController&)            = delete;
        SafaiaController& operator=(const SafaiaController&) = delete;

        void configure(const SafaiaControllerConfig& cfg);
        void setLogger(SafaiaLogFn fn) { logFn_ = std::move(fn); }

        // 启动 TCP server + UDP 发现；成功返回 true。
        bool start();
        void stop();

        bool isConnected() const { return state_.isConnected(); }
        bool isReady() const { return state_.isReady(); }
        int  boundPort() const { return boundPort_; }

        UiDebugState&       state() { return state_; }
        const UiDebugState& state() const { return state_; }

        // ── 高层 RPC（供 MCP 工具/GUI 调用；串行化、带超时）──
        RpcResult getControlTree(const std::string& rootPath);
        RpcResult getControlsData(const std::vector<std::string>& paths);

        // ── setter（fire-and-forget，无 ack）：返回是否成功发出帧 ──
        bool setSelectedControls(const std::vector<std::string>& paths);
        bool setBoundsVisible(bool visible);
        bool setControlVisible(const std::string& path, bool visible);
        bool setEnabled(bool enabled);

    private:
        void log(const std::string& level, const std::string& msg);

        // 网络
        bool startServer();
        void acceptLoop();
        void handleClient(void* clientSock); // 阻塞读循环直到断连
        bool sendFrame(int32_t protocolId, const nlohmann::json& payload);

        // 帧分发
        void onFrame(const Frame& f);
        void onCmdFrame(const std::string& payload);
        void onConfig(const nlohmann::json& cfg);

        // RPC：发送 uidebuger_call 并等待首个非带外响应。
        RpcResult callRpc(const std::string& func, const nlohmann::json& args);
        bool      sendUidebuger(const std::string& func, const nlohmann::json& args); // fire-and-forget
        void      deliverResponse(const nlohmann::json& inner);
        void      drainResponses();

        // enable 序列（独立线程：SetEnabled + 轮询 GetControlTree 直到就绪）。
        void runEnableSequence();

        SafaiaControllerConfig cfg_;
        UiDebugState           state_;
        DiscoverySender        discovery_;
        SafaiaLogFn            logFn_;

        void*      listenSock_ = nullptr;
        void*      clientSock_ = nullptr;
        mutable std::mutex clientMtx_; // 保护 clientSock_
        std::mutex sendMtx_;           // 串行化 socket 写
        int        boundPort_ = 0;

        std::atomic<bool>          running_{false};
        std::atomic<bool>          enableSpawned_{false};
        std::atomic<bool>          clientConnected_{false}; // 供 RPC 等待谓词无锁判定
        std::optional<std::thread> acceptThread_;
        std::optional<std::thread> enableThread_;

        ProtocolFramer framer_; // 仅 acceptLoop/handleClient 单线程使用

        // RPC 请求串行化（一次一个在途）+ 响应投递
        std::mutex                 rpcMtx_;
        std::mutex                 respMtx_;
        std::condition_variable    respCv_;
        std::queue<nlohmann::json> respQueue_; // 仅存非带外响应（handle != 4,7）
    };

} // namespace MCDevTool::Safaia
