#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <functional>
#include <map>
#include "./log_buffer.hpp"
#include "./mcp_tool_definitions.hpp"
#include <nlohmann/json.hpp>
#include <mcp_server.h>
#include <base64.hpp>
#include <mcdevtool/style.h>
#include <mcdevtool/safaia/controller.h>

namespace mcdk {

    // MCP服务器配置结构
    struct McpServerConfig {
        bool        enabled    = false;
        std::string serverIp   = "localhost";
        int         serverPort = 19133;

        // 各内置工具按工具名单独开关(缺省启用);未列出的工具默认启用。
        std::map<std::string, bool> toolEnabled;
        // 自定义工具组开关(整组:@mcp_tool 发现 + resync_custom_tools)。health_check 不受此约束,始终启用。
        bool customTools = true;

        bool isToolEnabled(const std::string& name) const {
            auto it = toolEnabled.find(name);
            return it == toolEnabled.end() ? true : it->second; // 缺省启用
        }

        // 是否有任一 UI 调试工具启用(决定是否启动 Safaia 控制器)。
        bool anyUiDebugEnabled() const {
            const char* uiNames[] = {
                mcp_tool_definitions::UiControlTreeName,    mcp_tool_definitions::UiControlGetDataName,
                mcp_tool_definitions::UiControlSearchName,  mcp_tool_definitions::UiLocateControlName,
                mcp_tool_definitions::UiDebugOverlayName,   mcp_tool_definitions::UiSetVisibleName,
                mcp_tool_definitions::UiGetSelectionName,   mcp_tool_definitions::UiWaitForSelectionName,
                mcp_tool_definitions::UiSetDebugEnabledName,
            };
            for (const char* n : uiNames) {
                if (isToolEnabled(n)) {
                    return true;
                }
            }
            return false;
        }
    };

    // 从JSON获取MCP服务器配置
    inline McpServerConfig getMcpServerConfigFromJson(const nlohmann::json& userConfig) {
        McpServerConfig config;
        auto            mcpJson = userConfig.value("mcp_server_config", nlohmann::json::object());
        if (mcpJson.is_object()) {
            config.enabled    = mcpJson.value("enabled", false);
            config.serverIp   = mcpJson.value("server_ip", "localhost");
            config.serverPort = mcpJson.value("server_port", 19133);
            // tools 段:每个内置工具名 -> bool(单独开关);特殊键 custom_tools 控制自定义工具组。
            // 缺省全 true(无 tools 段 / 未列出的工具均启用),向后兼容。
            auto toolsJson = mcpJson.value("tools", nlohmann::json::object());
            if (toolsJson.is_object()) {
                for (auto it = toolsJson.begin(); it != toolsJson.end(); ++it) {
                    if (!it.value().is_boolean()) {
                        continue;
                    }
                    if (it.key() == "custom_tools") {
                        config.customTools = it.value().get<bool>();
                    } else {
                        config.toolEnabled[it.key()] = it.value().get<bool>();
                    }
                }
            }
        }
        return config;
    }

    // 专为MCBE设计的MCP服务器
    class MCPServer {
    public:
        using CodeExecuteHandler = std::function<nlohmann::json(const std::string& code, bool isClient, bool directReturn)>;
        // 定义单次执行返回状态bool的Handler类型 无参数
        using SimpleHandler = std::function<bool()>;
        // 接收一个字符串参数的Handler类型（用于单个着色器重载）
        using StringParamHandler = std::function<bool(const std::string& param)>;

    private:
        McpServerConfig              config;
        std::shared_ptr<LogBuffer>   logBuffer;                 // 用于存储日志的缓冲区
        std::shared_ptr<LogBuffer>   errBuffer;                 // 用于存储错误日志的缓冲区
        std::shared_ptr<mcp::server> server;                    // MCP服务器实例
        CodeExecuteHandler           codeExecuteHandler;        // 代码执行处理器
        SimpleHandler                reloadGameHandler;         // 重载游戏处理器
        SimpleHandler                reloadShadersHandler;      // 重载着色器处理器
        SimpleHandler                reloadAddonAndGameHandler; // 重载插件和游戏处理器
        StringParamHandler           reloadOnceShadersHandler;  // 重载单个着色器处理器
        int                          mcPid = 0;                 // 存储Minecraft进程ID以供后续使用
        std::shared_ptr<MCDevTool::Safaia::SafaiaController> safaiaController; // UI 调试控制器（可空）

    public:
        MCPServer(const McpServerConfig& cfg) : config(cfg) {}
        MCPServer(McpServerConfig&& cfg) : config(std::move(cfg)) {}

        void setLogBuffer(std::shared_ptr<LogBuffer> buffer) { logBuffer = std::move(buffer); }
        void setErrBuffer(std::shared_ptr<LogBuffer> buffer) { errBuffer = std::move(buffer); }
        void setCodeExecuteHandler(CodeExecuteHandler handler) { codeExecuteHandler = std::move(handler); }
        void setReloadGameHandler(SimpleHandler handler) { reloadGameHandler = std::move(handler); }
        void setReloadShadersHandler(SimpleHandler handler) { reloadShadersHandler = std::move(handler); }
        void setReloadOnceShadersHandler(StringParamHandler handler) { reloadOnceShadersHandler = std::move(handler); }
        void setReloadAddonAndGameHandler(SimpleHandler handler) { reloadAddonAndGameHandler = std::move(handler); }
        void setMinecraftProcessId(int pid) { mcPid = pid; }
        void setSafaiaController(std::shared_ptr<MCDevTool::Safaia::SafaiaController> ctrl) {
            safaiaController = std::move(ctrl);
        }

        // ── 动态工具注册（供 CustomToolManager 使用；server 未启动时为空操作）──
        bool isServerRunning() const { return server != nullptr; }

        void registerDynamicTool(const mcp::tool& tool, mcp::tool_handler handler) {
            if (server) {
                server->register_tool(tool, std::move(handler));
            }
        }

        bool unregisterDynamicTool(const std::string& name) {
            return server ? server->unregister_tool(name) : false;
        }

        void notifyToolsListChanged() {
            if (server) {
                server->notify_tools_list_changed();
            }
        }

        static nlohmann::json _logVectorToJson(const std::vector<std::string>& logVector) {
            nlohmann::json jsonArray = nlohmann::json::array();
            for (const auto& log : logVector) {
                jsonArray.push_back({{"type", "text"}, {"text", log}});
            }
            return jsonArray;
        }

        // 构造单条文本结果（UI 调试工具统一使用）。
        static nlohmann::json _textResult(bool isError, const std::string& text) {
            return nlohmann::json{
                {"isError", isError},
                {"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}
            };
        }

        // 将 GetControlTree 响应树扁平化为 [{path,name,type,visible}]（对照 event_probe.flatten_tree）。
        static void _flattenTreeWalk(
            const nlohmann::json& node, const std::string& prefix, std::vector<nlohmann::json>& out, size_t limit
        ) {
            if (!node.is_object() || out.size() >= limit) {
                return;
            }
            if (node.contains("data") && node["data"].is_object()) {
                _flattenTreeWalk(node["data"], prefix, out, limit);
            }
            auto emit = [&](const std::string& path) {
                out.push_back({
                    {   "path",                  path},
                    {   "name",   node.value("name", std::string{})},
                    {   "type",   node.value("type", std::string{})},
                    {"visible", node.value("visible", true)},
                });
            };
            auto walkChildren = [&](const std::string& nextPrefix) {
                if (node.contains("controls") && node["controls"].is_array()) {
                    for (const auto& ch : node["controls"]) {
                        _flattenTreeWalk(ch, nextPrefix, out, limit);
                    }
                } else if (node.contains("children") && node["children"].is_array()) {
                    for (const auto& ch : node["children"]) {
                        _flattenTreeWalk(ch, nextPrefix, out, limit);
                    }
                }
            };

            std::string explicitPath;
            if (node.contains("path") && node["path"].is_string()) {
                explicitPath = node["path"].get<std::string>();
            }
            if (!explicitPath.empty() && explicitPath != "/") {
                emit(explicitPath);
                walkChildren(explicitPath);
                return;
            }
            std::string name = node.value("name", std::string{});
            if (!name.empty()) {
                std::string full = prefix.empty() ? ("/" + name) : (prefix + "/" + name);
                emit(full);
                walkChildren(full);
            } else {
                walkChildren(prefix);
            }
        }

        static std::vector<nlohmann::json> _flattenTree(const nlohmann::json& treeData, size_t limit) {
            std::vector<nlohmann::json> out;
            if (treeData.is_object()) {
                _flattenTreeWalk(treeData, "", out, limit);
            }
            return out;
        }

        static std::string _toLower(std::string s) {
            for (auto& c : s) {
                c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
            }
            return s;
        }

        // 初始化日志相关的工具
        void initLogTool() {
            mcp::tool logTool = mcp_tool_definitions::buildGetLatestLogsTool();

            server->register_tool(
                logTool,
                [this](const nlohmann::json& params, const std::string& /* session_id */) -> nlohmann::json {
                    size_t      maxCount = params.value("max_count", 100);
                    std::string order    = params.value("order", "asc");
                    if (!logBuffer) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", "Log buffer not set"}}})}
                        };
                    }
                    if (order == "desc") {
                        return _logVectorToJson(logBuffer->getLatestReversed(maxCount));
                    }
                    return _logVectorToJson(logBuffer->getLatest(maxCount));
                }
            );

            mcp::tool rangeLogTool = mcp_tool_definitions::buildGetLogRangeTool();

            server->register_tool(
                rangeLogTool,
                [this](const nlohmann::json& params, const std::string& /* session_id */) -> nlohmann::json {
                    size_t      startIndex = params.value("start_index", 0);
                    size_t      endIndex   = params.value("end_index", 100);
                    std::string order      = params.value("order", "asc");
                    if (!logBuffer) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", "Log buffer not set"}}})}
                        };
                    }
                    if (order == "desc") {
                        return _logVectorToJson(logBuffer->getRangeReversed(startIndex, endIndex));
                    }
                    return _logVectorToJson(logBuffer->getRange(startIndex, endIndex));
                }
            );

            // 错误日志查询工具
            // 与普通日志不同，错误日志仅包含stderr的输出，甚至不一定包含非py的错误信息，例如游戏JSON错误等，完整日志需要另外查询普通日志
            mcp::tool errLogTool = mcp_tool_definitions::buildGetLatestErrorLogsTool();

            server->register_tool(
                errLogTool,
                [this](const nlohmann::json& params, const std::string& /* session_id */) -> nlohmann::json {
                    size_t      maxCount = params.value("max_count", 100);
                    std::string order    = params.value("order", "asc");
                    if (!errBuffer) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array({{{"type", "text"}, {"text", "Error log buffer not set"}}})}
                        };
                    }
                    if (order == "desc") {
                        return _logVectorToJson(errBuffer->getLatestReversed(maxCount));
                    }
                    return _logVectorToJson(errBuffer->getLatest(maxCount));
                }
            );
        }

        // 初始化代码执行相关的工具
        void initCodeExecutionTool() {
            mcp::tool codeExecTool = mcp_tool_definitions::buildExecuteCodeTool();

            server->register_tool(
                codeExecTool,
                [this](const nlohmann::json& params, const std::string& session_id) -> nlohmann::json {
                    if (!codeExecuteHandler) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array({{{"type", "text"}, {"text", "Code execution handler not set"}}})}
                        };
                    }

                    std::string code         = params.value("code", "");
                    bool        isClient     = params.value("is_client", true);
                    bool        directReturn = params.value("direct_return", true);

                    return codeExecuteHandler(code, isClient, directReturn);
                }
            );
        }

        // 初始化游戏相关工具
        void initGameTools() {
            // 提供重新加载游戏的工具
            mcp::tool reloadGameTool = mcp_tool_definitions::buildReloadGameTool();
            server->register_tool(
                reloadGameTool,
                [this](const nlohmann::json& /* params */, const std::string& /* session_id */) -> nlohmann::json {
                    if (reloadGameHandler) {
                        if (reloadGameHandler()) {
                            return nlohmann::json{
                                {"isError", false},
                                {"content",
                                 nlohmann::json::array({{{"type", "text"}, {"text", "Game reload triggered"}}})}
                            };
                        } else {
                            // 也许玩家不在游戏中，无法执行重载
                            return nlohmann::json{
                                {"isError", true},
                                {"content",
                                 nlohmann::json::array(
                                     {{{"type", "text"},
                                       {"text", "Game reload failed. Player may not be in the game."}}}
                                 )}
                            };
                        }
                    } else {
                        return nlohmann::json{
                            {"isError", true},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", "Reload handler not set"}}})}
                        };
                    }
                }
            );

            // 重新加载游戏以及addon数据（增量贴图/音频之类时需要使用该工具以确保资源被正确重新加载）
            mcp::tool reloadAddonAndGameTool = mcp_tool_definitions::buildReloadAddonAndGameTool();
            server->register_tool(
                reloadAddonAndGameTool,
                [this](const nlohmann::json& /* params */, const std::string& /* session_id */) -> nlohmann::json {
                    if (reloadAddonAndGameHandler) {
                        if (reloadAddonAndGameHandler()) {
                            return nlohmann::json{
                                {"isError", false},
                                {"content",
                                 nlohmann::json::array(
                                     {{{"type", "text"}, {"text", "Addon and game reload triggered"}}}
                                 )}
                            };
                        } else {
                            // 也许玩家不在游戏中，无法执行重载
                            return nlohmann::json{
                                {"isError", true},
                                {"content",
                                 nlohmann::json::array(
                                     {{{"type", "text"},
                                       {"text", "Addon and game reload failed. Player may not be in the game."}}}
                                 )}
                            };
                        }
                    } else {
                        return nlohmann::json{
                            {"isError", true},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", "Reload handler not set"}}})}
                        };
                    }
                }
            );

            // 重新编译着色器的工具（完整重新编译着色器耗时较长，不建议频繁调用，也不建议盲等完成，可以由用户确认完成后再验证结果）
            mcp::tool reloadShadersTool = mcp_tool_definitions::buildReloadAllShadersTool();
            server->register_tool(
                reloadShadersTool,
                [this](const nlohmann::json& /* params */, const std::string& /* session_id */) -> nlohmann::json {
                    if (reloadShadersHandler) {
                        if (reloadShadersHandler()) {
                            return nlohmann::json{
                                {"isError", false},
                                {"content",
                                 nlohmann::json::array({{{"type", "text"}, {"text", "Shader reload triggered"}}})}
                            };
                        } else {
                            // 也许玩家不在游戏中，无法执行重载
                            return nlohmann::json{
                                {"isError", true},
                                {"content",
                                 nlohmann::json::array(
                                     {{{"type", "text"},
                                       {"text", "Shader reload failed. Player may not be in the game."}}}
                                 )}
                            };
                        }
                    } else {
                        return nlohmann::json{
                            {"isError", true},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", "Reload handler not set"}}})}
                        };
                    }
                }
            );

            // 重新编译单个着色器工具  描述：编译单个着色器文件如："entity.fragment"
            // 以加速编译测试，但如果同时涉及多个文件修改可能会因为依赖关系导致错误，此时应考虑reload_all_shaders
            mcp::tool reloadOnceShaderTool = mcp_tool_definitions::buildReloadSingleShaderTool();
            server->register_tool(
                reloadOnceShaderTool,
                [this](const nlohmann::json& params, const std::string& /* session_id */) -> nlohmann::json {
                    std::string fileName = params.value("file_name", "");
                    if (fileName.empty()) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array({{{"type", "text"}, {"text", "File name parameter is required"}}})}
                        };
                    }
                    if (reloadOnceShadersHandler) {
                        if (reloadOnceShadersHandler(fileName)) {
                            return nlohmann::json{
                                {"isError", false},
                                {"content",
                                 nlohmann::json::array(
                                     {{{"type", "text"}, {"text", "Single shader reload triggered"}}}
                                 )}
                            };
                        } else {
                            // 也许玩家不在游戏中，无法执行重载
                            return nlohmann::json{
                                {"isError", true},
                                {"content",
                                 nlohmann::json::array(
                                     {{{"type", "text"},
                                       {"text",
                                        "Single shader reload failed. Player may not be in the game or file name may "
                                        "be incorrect."}}}
                                 )}
                            };
                        }
                    } else {
                        return nlohmann::json{
                            {"isError", true},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", "Reload handler not set"}}})}
                        };
                    }
                }
            );
        }

        // 初始化游戏窗口工具（如获取画面，模拟点击）
        void initGameWindowTools() {
            // 截图工具：捕获游戏窗口画面，返回 480p JPEG base64 图片
            mcp::tool captureTool = mcp_tool_definitions::buildCaptureGameWindowTool();

            server->register_tool(
                captureTool,
                [this](const nlohmann::json& /* params */, const std::string& /* session_id */) -> nlohmann::json {
                    if (mcPid <= 0) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array(
                                 {{{"type", "text"},
                                   {"text", "Game process ID not set. The game window does not exist."}}}
                             )}
                        };
                    }

                    auto result = MCDevTool::Style::captureMinecraftWindow480p(mcPid);
                    if (!result.has_value() || result->empty()) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array(
                                 {{{"type", "text"},
                                   {"text",
                                    "Failed to capture game window. "
                                    "The game window does not exist or is minimized."}}}
                             )}
                        };
                    }

                    // 将 JPEG 数据编码为 base64
                    std::string b64 = base64::encode(reinterpret_cast<const char*>(result->data()), result->size());

                    return nlohmann::json{
                        {"isError", false},
                        {"content",
                         nlohmann::json::array({{{"type", "image"}, {"data", b64}, {"mimeType", "image/jpeg"}}})}
                    };
                }
            );

            // 点击工具：模拟点击游戏窗口指定位置
            mcp::tool clickTool = mcp_tool_definitions::buildClickGameWindowTool();

            server->register_tool(
                clickTool,
                [this](const nlohmann::json& params, const std::string& /* session_id */) -> nlohmann::json {
                    if (mcPid <= 0) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array(
                                 {{{"type", "text"},
                                   {"text", "Game process ID not set. The game window does not exist."}}}
                             )}
                        };
                    }

                    double x = params.value("x", -1.0);
                    double y = params.value("y", -1.0);

                    if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array(
                                 {{{"type", "text"},
                                   {"text", "Invalid coordinates. x and y must be between 0.0 and 1.0."}}}
                             )}
                        };
                    }

                    bool success = MCDevTool::Style::clickMinecraftWindowAt(mcPid, x, y);
                    if (!success) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array(
                                 {{{"type", "text"},
                                   {"text",
                                    "Failed to click on game window. "
                                    "The game window does not exist or is minimized."}}}
                             )}
                        };
                    }

                    return nlohmann::json{
                        {"isError", false},
                        {"content",
                         nlohmann::json::array(
                             {{{"type", "text"},
                               {"text",
                                "Click performed at (" + std::to_string(x) + ", " + std::to_string(y)
                                    + "). Use capture_game_window to verify the result."}}}
                         )}
                    };
                }
            );
        }

        // 初始化 UI 调试工具（Safaia 控制器）。handler 在调用时检查控制器是否可用/已连接。
        void initUiDebugTools() {
            // ui_control_tree：读控件树（缓存或 GetControlTree，支持子路径与强制刷新）。
            server->register_tool(
                mcp_tool_definitions::buildUiControlTreeTool(),
                [this](const nlohmann::json& params, const std::string&) -> nlohmann::json {
                    if (!safaiaController) {
                        return _textResult(true, "Safaia UI debugger controller is not available.");
                    }
                    if (!safaiaController->isConnected()) {
                        return _textResult(
                            true,
                            "Game UI debugger not connected. Ensure the game is running and was launched by mcdk."
                        );
                    }
                    std::string rootPath = params.value("root_path", std::string("/"));
                    bool        force    = params.value("force_refresh", false);
                    if (!force) {
                        auto cached = safaiaController->state().getCachedTree(rootPath);
                        if (cached.has_value()) {
                            return _textResult(false, cached->dump(2));
                        }
                    }
                    // 临时启用 UI 调试取树，调用结束自动关闭（除非已显式保持开启）。
                    MCDevTool::Safaia::SafaiaController::DebugSession dbg(*safaiaController);
                    auto r = safaiaController->getControlTree(rootPath);
                    if (r.ok && r.handle == MCDevTool::Safaia::RPCHandles::GetControlTree && r.success) {
                        return _textResult(false, r.data.dump(2));
                    }
                    std::string why = r.timeout ? "timed out (illegal root path or UI debugger not ready)"
                                                : ("unexpected response: " + r.handleName());
                    if (!r.error.empty()) {
                        why = r.error;
                    }
                    return _textResult(true, "ui_control_tree failed: " + why);
                }
            );

            // ui_control_get_data：批量取控件属性。
            server->register_tool(
                mcp_tool_definitions::buildUiControlGetDataTool(),
                [this](const nlohmann::json& params, const std::string&) -> nlohmann::json {
                    if (!safaiaController) {
                        return _textResult(true, "Safaia UI debugger controller is not available.");
                    }
                    if (!safaiaController->isConnected()) {
                        return _textResult(true, "Game UI debugger not connected.");
                    }
                    std::vector<std::string> paths;
                    if (params.contains("paths") && params["paths"].is_array()) {
                        for (const auto& p : params["paths"]) {
                            if (p.is_string()) {
                                paths.push_back(p.get<std::string>());
                            }
                        }
                    }
                    if (paths.empty()) {
                        return _textResult(true, "Parameter 'paths' must be a non-empty array of control paths.");
                    }
                    MCDevTool::Safaia::SafaiaController::DebugSession dbg(*safaiaController);
                    auto r = safaiaController->getControlsData(paths);
                    if (r.ok && r.handle == MCDevTool::Safaia::RPCHandles::GetControlsData && r.success) {
                        return _textResult(false, r.data.dump(2));
                    }
                    std::string why = r.timeout ? "timed out" : ("unexpected response: " + r.handleName());
                    if (!r.error.empty()) {
                        why = r.error;
                    }
                    return _textResult(true, "ui_control_get_data failed: " + why);
                }
            );

            // ui_control_search：在缓存/拉取的树上本地过滤。
            server->register_tool(
                mcp_tool_definitions::buildUiControlSearchTool(),
                [this](const nlohmann::json& params, const std::string&) -> nlohmann::json {
                    if (!safaiaController) {
                        return _textResult(true, "Safaia UI debugger controller is not available.");
                    }
                    if (!safaiaController->isConnected()) {
                        return _textResult(true, "Game UI debugger not connected.");
                    }
                    std::string keyword = params.value("keyword", std::string{});
                    std::string by      = params.value("by", std::string("name"));
                    size_t      limit   = static_cast<size_t>(params.value("limit", 50));
                    if (keyword.empty()) {
                        return _textResult(true, "Parameter 'keyword' is required.");
                    }
                    if (by != "name" && by != "type" && by != "path") {
                        by = "name";
                    }
                    // 取整树（缓存优先）。
                    nlohmann::json tree;
                    auto           cached = safaiaController->state().getCachedTree("/");
                    if (cached.has_value()) {
                        tree = *cached;
                    } else {
                        MCDevTool::Safaia::SafaiaController::DebugSession dbg(*safaiaController);
                        auto r = safaiaController->getControlTree("/");
                        if (!(r.ok && r.handle == MCDevTool::Safaia::RPCHandles::GetControlTree && r.success)) {
                            std::string why = r.timeout ? "timed out" : ("unexpected response: " + r.handleName());
                            return _textResult(true, "ui_control_search failed to obtain tree: " + why);
                        }
                        tree = r.data;
                    }
                    auto           flat = _flattenTree(tree, 5000);
                    std::string    kw   = _toLower(keyword);
                    nlohmann::json hits = nlohmann::json::array();
                    for (const auto& node : flat) {
                        std::string field = node.value(by, std::string{});
                        if (_toLower(field).find(kw) != std::string::npos) {
                            hits.push_back(node);
                            if (hits.size() >= limit) {
                                break;
                            }
                        }
                    }
                    nlohmann::json result{
                        {     "by",          by},
                        {"keyword",     keyword},
                        {  "count", hits.size()},
                        {  "matches",      hits},
                    };
                    return _textResult(false, result.dump(2));
                }
            );

            // ui_locate_control：红框高亮（fire-and-forget）。
            server->register_tool(
                mcp_tool_definitions::buildUiLocateControlTool(),
                [this](const nlohmann::json& params, const std::string&) -> nlohmann::json {
                    if (!safaiaController || !safaiaController->isConnected()) {
                        return _textResult(true, "Game UI debugger not connected.");
                    }
                    std::vector<std::string> paths;
                    if (params.contains("paths") && params["paths"].is_array()) {
                        for (const auto& p : params["paths"]) {
                            if (p.is_string()) {
                                paths.push_back(p.get<std::string>());
                            }
                        }
                    }
                    if (paths.empty()) {
                        return _textResult(true, "Parameter 'paths' must be a non-empty array.");
                    }
                    // 保持调试开启，使红框在 capture_game_window 时可见。
                    safaiaController->setDebugHeld(true);
                    bool ok = safaiaController->setSelectedControls(paths);
                    if (!ok) {
                        return _textResult(true, "Failed to send SetSelectedControls (game disconnected?).");
                    }
                    return _textResult(
                        false,
                        "Highlighted " + std::to_string(paths.size())
                            + " control(s) with a red box. UI debug mode is now ON so it stays visible — confirm with "
                              "capture_game_window, then call ui_set_debug_enabled(enabled=false) to return to normal play."
                    );
                }
            );

            // ui_debug_overlay：全屏轮廓叠加（fire-and-forget）。
            server->register_tool(
                mcp_tool_definitions::buildUiDebugOverlayTool(),
                [this](const nlohmann::json& params, const std::string&) -> nlohmann::json {
                    if (!safaiaController || !safaiaController->isConnected()) {
                        return _textResult(true, "Game UI debugger not connected.");
                    }
                    bool visible = params.value("visible", false);
                    if (visible) {
                        safaiaController->setDebugHeld(true); // 先保持调试开启再显示轮廓
                    }
                    bool ok = safaiaController->setBoundsVisible(visible);
                    if (!visible) {
                        safaiaController->setDebugHeld(false); // 隐藏轮廓同时退出调试，恢复正常交互
                    }
                    if (!ok) {
                        return _textResult(true, "Failed to send SetBoundsVisible (game disconnected?).");
                    }
                    return _textResult(
                        false,
                        std::string("Bounds overlay ")
                            + (visible ? "enabled; UI debug mode is ON — confirm with capture_game_window, then call "
                                         "ui_debug_overlay(visible=false) to return to normal play"
                                       : "disabled; UI debug mode returned to normal play")
                            + "."
                    );
                }
            );

            // ui_set_visible：单控件显隐（fire-and-forget）。
            server->register_tool(
                mcp_tool_definitions::buildUiSetVisibleTool(),
                [this](const nlohmann::json& params, const std::string&) -> nlohmann::json {
                    if (!safaiaController || !safaiaController->isConnected()) {
                        return _textResult(true, "Game UI debugger not connected.");
                    }
                    std::string path = params.value("path", std::string{});
                    if (path.empty()) {
                        return _textResult(true, "Parameter 'path' is required.");
                    }
                    bool visible = params.value("visible", true);
                    MCDevTool::Safaia::SafaiaController::DebugSession dbg(*safaiaController);
                    bool ok      = safaiaController->setControlVisible(path, visible);
                    if (!ok) {
                        return _textResult(true, "Failed to send SetControlVisible (game disconnected?).");
                    }
                    return _textResult(
                        false,
                        "Set control '" + path + "' visible=" + (visible ? "true" : "false")
                            + " (fire-and-forget). Re-check with ui_control_get_data."
                    );
                }
            );

            // ui_get_selection：读最后选中快照。
            server->register_tool(
                mcp_tool_definitions::buildUiGetSelectionTool(),
                [this](const nlohmann::json&, const std::string&) -> nlohmann::json {
                    if (!safaiaController) {
                        return _textResult(true, "Safaia UI debugger controller is not available.");
                    }
                    auto           paths = safaiaController->state().lastSelection();
                    nlohmann::json arr   = nlohmann::json::array();
                    for (const auto& p : paths) {
                        arr.push_back(p);
                    }
                    nlohmann::json result{
                        {           "seq", safaiaController->state().selectionSeq()},
                        {"current_screen",   safaiaController->state().currentScreen()},
                        {     "selection",                                       arr},
                    };
                    if (paths.empty()) {
                        result["note"] = "No selection captured yet. Use ui_wait_for_selection and click a control.";
                    }
                    return _textResult(false, result.dump(2));
                }
            );

            // ui_wait_for_selection：阻塞等待下一个去重后的选中事件。
            server->register_tool(
                mcp_tool_definitions::buildUiWaitForSelectionTool(),
                [this](const nlohmann::json& params, const std::string&) -> nlohmann::json {
                    if (!safaiaController) {
                        return _textResult(true, "Safaia UI debugger controller is not available.");
                    }
                    if (!safaiaController->isConnected()) {
                        return _textResult(true, "Game UI debugger not connected.");
                    }
                    int timeoutSec = params.value("timeout", 30);
                    if (timeoutSec <= 0) {
                        timeoutSec = 30;
                    }
                    if (timeoutSec > 120) {
                        timeoutSec = 120;
                    }
                    // 等待期间临时启用调试，使游戏内点击能产生 ControlSelectionChanged；结束后自动关闭。
                    MCDevTool::Safaia::SafaiaController::DebugSession dbg(*safaiaController);
                    auto&                    st    = safaiaController->state();
                    uint64_t                 since = st.selectionSeq();
                    std::vector<std::string> out;
                    uint64_t                 outSeq = 0;
                    bool got = st.waitForSelection(since, timeoutSec * 1000, out, outSeq);
                    if (!got) {
                        return _textResult(
                            true,
                            "No control selected within " + std::to_string(timeoutSec) + "s (timeout)."
                        );
                    }
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto& p : out) {
                        arr.push_back(p);
                    }
                    nlohmann::json result{
                        {      "seq",                            outSeq},
                        {"selection",                               arr},
                    };
                    return _textResult(false, result.dump(2));
                }
            );

            // ui_set_debug_enabled：显式保持/退出 UI 调试模式。默认关闭=游戏正常交互；
            // 需要持久可见的调试效果（红框/轮廓）或连续多次调试操作时，先 enabled=true，完后 enabled=false。
            server->register_tool(
                mcp_tool_definitions::buildUiSetDebugEnabledTool(),
                [this](const nlohmann::json& params, const std::string&) -> nlohmann::json {
                    if (!safaiaController) {
                        return _textResult(true, "Safaia UI debugger controller is not available.");
                    }
                    if (!safaiaController->isConnected()) {
                        return _textResult(true, "Game UI debugger not connected.");
                    }
                    bool enabled = params.value("enabled", false);
                    safaiaController->setDebugHeld(enabled);
                    return _textResult(
                        false,
                        enabled ? "UI debug mode held ON. Clicks now select controls instead of operating the UI; "
                                  "visualization tools (ui_locate_control / ui_debug_overlay) will persist. "
                                  "Call ui_set_debug_enabled(enabled=false) to return to normal play."
                                : "UI debug mode released; game returned to normal interaction."
                    );
                }
            );
        }

        // 初始化所有工具（每个工具按 .mcdev.json 的 mcp_server_config.tools 单独开关,缺省启用）
        void initTools() {
            initLogTool();
            initCodeExecutionTool();
            initGameTools();
            initGameWindowTools();
            initUiDebugTools();
            // 应用单独开关:注销被显式禁用的内置工具(未列出的保持启用)。
            // health_check 与自定义工具由 CustomToolManager 负责,不在此列。
            for (const auto& [name, enabled] : config.toolEnabled) {
                if (!enabled) {
                    server->unregister_tool(name);
                }
            }
        }

        // 启动MCP服务器
        void start() {
            if (!config.enabled || server.get() != nullptr) {
                return;
            }
            mcp::server::configuration srv_conf;
            srv_conf.host = config.serverIp;
            srv_conf.port = config.serverPort;
            server        = std::make_shared<mcp::server>(srv_conf);
            server->set_server_info("Minecraft(BE) MCP Server(MCDK)", "0.1.0");
            // 注册API
            initTools();
            server->start(false); // 非阻塞启动
        }

        // 停止MCP服务器
        void stop() {
            if (!config.enabled || server.get() == nullptr) {
                return;
            }
            server->stop();
            server.reset();
        }
    };
} // namespace mcdk