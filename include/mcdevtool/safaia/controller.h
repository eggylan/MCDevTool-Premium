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
#include <string_view>
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

    // 游戏日志回调（可选）：MCProtocol::message(协议 4) 的原始 payload。
    // 与 SafaiaLogFn(控制器自身连接/握手/错误日志)区分：此回调专用于转发游戏侧日志，
    // controller 不在此做 [Python] 过滤、颜色判断或 MCP 缓冲(交由 GameLogProcessor)。
    using SafaiaMessageFn = std::function<void(std::string_view payload)>;

    class SafaiaController {
    public:
        SafaiaController() = default;
        ~SafaiaController();

        SafaiaController(const SafaiaController&)            = delete;
        SafaiaController& operator=(const SafaiaController&) = delete;

        void configure(const SafaiaControllerConfig& cfg);
        void setLogger(SafaiaLogFn fn) { logFn_ = std::move(fn); }

        // 设置游戏日志(协议 4)回调。未设置时静默丢弃协议 4，不影响 UI RPC。
        void setMessageHandler(SafaiaMessageFn fn) { messageFn_ = std::move(fn); }

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

        // ── 按需启用 UI 调试（默认关闭，游戏保持正常交互）──
        // 引用计数 + 过渡互斥：首个会话启用并等待就绪，最后一个会话退出时关闭。
        bool beginDebugSession(); // 返回是否已就绪
        void endDebugSession();

        // 显式“保持”调试开启（持有一个不配对的会话引用），供需要持久可见效果的场景
        // （如 SetBoundsVisible/SetSelectedControls 后用 capture_game_window 截图）。
        // on=true 持有；on=false 释放。幂等。
        void setDebugHeld(bool on);
        bool isDebugHeld() const { return heldSession_.load(); }

        // RAII 守卫：进入确保 UI 调试启用（含就绪等待），离开计数归零时关闭。
        // 让游戏默认处于正常（非调试）模式，仅在 UI 调试工具调用期间临时开启。
        class DebugSession {
        public:
            explicit DebugSession(SafaiaController& c) : c_(&c), ready_(c.beginDebugSession()) {}
            ~DebugSession() { c_->endDebugSession(); }
            DebugSession(const DebugSession&)            = delete;
            DebugSession& operator=(const DebugSession&) = delete;
            bool ready() const { return ready_; }

        private:
            SafaiaController* c_;
            bool              ready_;
        };

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

        // 按需启用：SetEnabled(true) + 轮询 GetControlTree 直到就绪（约 2s 内）。
        bool performEnableAndWait();

        SafaiaControllerConfig cfg_;
        UiDebugState           state_;
        DiscoverySender        discovery_;
        SafaiaLogFn            logFn_;
        SafaiaMessageFn        messageFn_;

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

        // 按需启用状态（引用计数 + 过渡互斥）。默认关闭。
        std::mutex              enableMtx_;
        std::condition_variable enableCv_;
        int                     enableRefCount_      = 0;
        bool                    debugEnabled_        = false;
        bool                    enableTransitioning_ = false;
        std::atomic<bool>       heldSession_{false}; // 显式保持开启（持有一个引用）
    };

} // namespace MCDevTool::Safaia
