#pragma once
// 自定义 MCP 工具管理器（mcdk 侧）。
// 轮询 IPC 客户端连接状态，在游戏连上/重连时拉取 list_custom_tools，
// 把每个自定义工具注册成一等 MCP 工具（动态注册/注销 + tools/list_changed）。
// 与 mcp_server.hpp 同例为 header-only：均依赖 mcp::server，由 mcdk 可执行文件链接 cpp-mcp，无需改 CMake。
#include <atomic>
#include <thread>
#include <chrono>
#include <set>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <nlohmann/json.hpp>
#include <mcp_server.h>
#include <mcp_tool.h>
#include <mcdevtool/debug.h>
#include "./mcp_server.hpp"

namespace mcdk {

    // 把游戏侧 list_custom_tools 的工具集映射为一等 MCP 工具，并随游戏连接生命周期维护。
    class CustomToolManager {
    public:
        using Logger = std::function<void(const std::string& level, const std::string& msg)>;

        CustomToolManager()                                    = default;
        CustomToolManager(const CustomToolManager&)            = delete;
        CustomToolManager& operator=(const CustomToolManager&) = delete;
        ~CustomToolManager() { stop(); }

        void configure(MCPServer* mcp, std::shared_ptr<MCDevTool::Debug::DebugIPCServer> ipc) {
            mcpServer_ = mcp;
            ipcServer_ = std::move(ipc);
        }
        void setLogger(Logger l) { logger_ = std::move(l); }

        // 是否注册用户(非内置)自定义工具。false 时仅注册 builtin(如 health_check)。
        void setIncludeUserTools(bool b) { includeUserTools_ = b; }

        void start() {
            if (!mcpServer_ || !ipcServer_) {
                return;
            }
            if (running_.exchange(true)) {
                return;
            }
            pollThread_ = std::thread([this] { pollLoop(); });
        }

        void stop() {
            if (!running_.exchange(false)) {
                return;
            }
            if (pollThread_.joinable()) {
                pollThread_.join();
            }
            // 注销全部动态工具
            std::lock_guard<std::mutex> sync(syncMutex_);
            for (const auto& name : registered_) {
                if (mcpServer_) {
                    mcpServer_->unregisterDynamicTool(name);
                }
            }
            registered_.clear();
        }

        // 手动触发一次同步（供 resync_custom_tools MCP 工具/未来 GUI 使用；线程安全）。
        void syncNow() { doSync(); }

        // 当前已注册的自定义工具名（线程安全快照）。
        std::vector<std::string> registeredNames() {
            std::lock_guard<std::mutex> sync(syncMutex_);
            return std::vector<std::string>(registered_.begin(), registered_.end());
        }

    private:
        void log(const std::string& level, const std::string& msg) {
            if (logger_) {
                logger_(level, msg);
            }
        }

        // 可中断 sleep：把一段等待切成 100ms 片，running_ 置否即提前返回。
        void interruptibleSleep(int totalMs) {
            int slept = 0;
            while (slept < totalMs && running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                slept += 100;
            }
        }

        void pollLoop() {
            bool wasConnected = false;
            while (running_.load()) {
                bool connected = ipcServer_ && ipcServer_->getClientCount() > 0;
                if (connected && !wasConnected) {
                    // 上升沿：等 MOD init/IPCSystem 就绪后再拉取
                    interruptibleSleep(1500);
                    if (running_.load()) {
                        doSync();
                    }
                } else if (!connected && wasConnected) {
                    log("info", "Game disconnected; custom tools kept until next sync.");
                }
                wasConnected = connected;
                interruptibleSleep(1000);
            }
        }

        // 从 {name,description,params:[{name,type,description,required}]} 构建 mcp::tool。
        static mcp::tool buildTool(const nlohmann::json& t) {
            std::string       name = t.value("name", std::string{});
            std::string       desc = t.value("description", std::string{});
            mcp::tool_builder b(name);
            b.with_description(desc);
            if (t.contains("params") && t["params"].is_array()) {
                for (const auto& p : t["params"]) {
                    if (!p.is_object()) {
                        continue;
                    }
                    std::string pname = p.value("name", std::string{});
                    if (pname.empty()) {
                        continue;
                    }
                    std::string pdesc = p.value("description", std::string{});
                    std::string ptype = p.value("type", std::string("string"));
                    bool        req   = p.value("required", false);
                    if (ptype == "number") {
                        b.with_number_param(pname, pdesc, req);
                    } else if (ptype == "boolean") {
                        b.with_boolean_param(pname, pdesc, req);
                    } else if (ptype == "array") {
                        b.with_array_param(pname, pdesc, "string", req);
                    } else if (ptype == "object") {
                        b.with_object_param(pname, pdesc, nlohmann::json::object(), req);
                    } else {
                        b.with_string_param(pname, pdesc, req);
                    }
                }
            }
            b.with_open_world_hint(true); // 自定义工具与游戏环境交互
            return b.build();
        }

        // 为某自定义工具构建 MCP handler：经 IPC call_tool 派发到游戏侧，取回 return_string 包成文本。
        mcp::tool_handler makeHandler(const std::string& toolName) {
            auto        ipc  = ipcServer_;
            std::string name = toolName;
            return [ipc, name](const nlohmann::json& args, const std::string&) -> nlohmann::json {
                auto textResult = [](bool isErr, const std::string& text) -> nlohmann::json {
                    return nlohmann::json{
                        {"isError", isErr},
                        {"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}
                    };
                };
                if (!ipc || ipc->getClientCount() == 0) {
                    return textResult(true, "Custom tool '" + name + "' failed: game not connected.");
                }
                nlohmann::json params = {
                    {  "name",                                                   name},
                    {"params", args.is_object() ? args : nlohmann::json::object()}
                };
                auto res = ipc->requestJson("call_tool", params.dump(), 15000);
                if (!res.success) {
                    return textResult(true, "Custom tool '" + name + "' IPC failed: " + res.errorMessage);
                }
                auto resp = nlohmann::json::parse(res.responseJson, nullptr, false);
                if (resp.is_discarded() || !resp.is_object()) {
                    return textResult(true, "Custom tool '" + name + "' invalid response: " + res.responseJson);
                }
                if (!resp.value("ok", false)) {
                    std::string msg = resp.dump();
                    if (resp.contains("error") && resp["error"].is_object()) {
                        msg = resp["error"].value("message", msg);
                    }
                    return textResult(true, "Custom tool '" + name + "' error: " + msg);
                }
                auto        result = resp.value("result", nlohmann::json::object());
                std::string s;
                if (result.is_object() && result.contains("return_string")) {
                    const auto& rs = result["return_string"];
                    s              = rs.is_string() ? rs.get<std::string>() : rs.dump();
                } else {
                    s = result.dump(2);
                }
                return textResult(false, s);
            };
        }

        void doSync() {
            if (!mcpServer_ || !ipcServer_) {
                return;
            }
            if (ipcServer_->getClientCount() == 0) {
                return;
            }
            // 串行化 doSync（poll 线程与 syncNow 调用方互斥），保护 registered_ 与注册操作。
            std::lock_guard<std::mutex> sync(syncMutex_);
            auto res = ipcServer_->requestJson("list_custom_tools", "{}", 5000);
            if (!res.success) {
                log("warn", "list_custom_tools failed: " + res.errorMessage);
                return;
            }
            auto resp = nlohmann::json::parse(res.responseJson, nullptr, false);
            if (resp.is_discarded() || !resp.is_object() || !resp.value("ok", false)) {
                log("warn", "list_custom_tools bad response: " + res.responseJson);
                return;
            }
            auto result   = resp.value("result", nlohmann::json::object());
            auto toolsArr = result.value("tools", nlohmann::json::array());
            if (!toolsArr.is_array()) {
                return;
            }

            std::set<std::string> desired;
            bool                  changed = false;

            // 注册/重注册期望集合中的全部工具（register_tool 覆盖语义安全）。
            for (const auto& t : toolsArr) {
                if (!t.is_object()) {
                    continue;
                }
                std::string name = t.value("name", std::string{});
                if (name.empty()) {
                    continue;
                }
                // 内置工具(如 health_check)始终注册;用户工具受 includeUserTools_ 约束。
                bool builtin = t.value("builtin", false);
                if (!builtin && !includeUserTools_) {
                    continue;
                }
                desired.insert(name);
                mcpServer_->registerDynamicTool(buildTool(t), makeHandler(name));
                if (registered_.find(name) == registered_.end()) {
                    registered_.insert(name);
                    changed = true;
                }
            }

            // 注销不再出现的工具。
            std::vector<std::string> toRemove;
            for (const auto& name : registered_) {
                if (desired.find(name) == desired.end()) {
                    toRemove.push_back(name);
                }
            }
            for (const auto& name : toRemove) {
                mcpServer_->unregisterDynamicTool(name);
                registered_.erase(name);
                changed = true;
            }

            if (changed) {
                mcpServer_->notifyToolsListChanged();
            }
            log("info", "Custom tools synced: " + std::to_string(registered_.size()) + " registered.");

            // 上报扫描错误（若有）。
            if (result.contains("errors") && result["errors"].is_array() && !result["errors"].empty()) {
                log(
                    "warn",
                    "Custom tool scan reported " + std::to_string(result["errors"].size()) + " error(s)."
                );
            }
        }

        MCPServer*                                        mcpServer_ = nullptr;
        std::shared_ptr<MCDevTool::Debug::DebugIPCServer> ipcServer_;
        Logger                                            logger_;
        std::atomic<bool>                                 running_{false};
        std::atomic<bool>                                 includeUserTools_{true}; // 是否注册用户(非内置)工具
        std::thread                                       pollThread_;
        std::mutex                                        syncMutex_;  // 串行化 doSync + 保护 registered_
        std::set<std::string>                             registered_; // doSync/stop/registeredNames 在 syncMutex_ 下访问
    };

} // namespace mcdk
