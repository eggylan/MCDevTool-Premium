#pragma once

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <mcdevtool/debug.h>
#include <mcdevtool/env.h>
#include <mcdevtool/safaia/controller.h>
#include <mcdevtool/utils.h>

#include "./async_log_pump.hpp"
#include "./config.hpp"
#include "./console.hpp"
#include "./custom_tool_manager.hpp"
#include "./game_log_processor.hpp"
#include "./hotreload.hpp"
#include "./log_buffer.hpp"
#include "./mcp_server.hpp"
#include "./mod_dir_config.hpp"
#include "./style_processor.hpp"

#ifndef _MCDEV_LOG_OUTPUT_ENDL
#define _MCDEV_LOG_OUTPUT_ENDL "\n"
#endif

#ifdef MCDEV_LOG_FORCE_FLUSH_ENDL
#undef _MCDEV_LOG_OUTPUT_ENDL
#define _MCDEV_LOG_OUTPUT_ENDL std::endl
#endif

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace mcdk {

    class CoreServices {
    public:
        using ConsoleSink = GameLogProcessor::ConsoleSink;

    private:
        McpServerConfig      mcpConfig_;
        SafaiaUserSettings  safaiaSettings_;
        bool                filterPython_ = true;
        mutable std::mutex  consoleSinkMutex_;
        ConsoleSink         consoleSink_;
        bool                shutdown_ = false;

    public:
        explicit CoreServices(const nlohmann::json& userConfig, ConsoleSink consoleSink = defaultConsoleSink())
        : mcpConfig_(getMcpServerConfigFromJson(userConfig)),
          safaiaSettings_(getSafaiaUserSettingsFromJson(userConfig)),
          filterPython_(userConfig.value("include_debug_mod", true)),
          ipc(MCDevTool::Debug::createDebugServer()),
          logBuffer(std::make_shared<LogBuffer>(1000, 250)),
          errBuffer(std::make_shared<LogBuffer>(1000, 400)),
          mcp(std::make_unique<MCPServer>(mcpConfig_)),
          safaia(std::make_shared<MCDevTool::Safaia::SafaiaController>()),
          styleProcessor(0, userConfig) {
            setConsoleSinkFanout(std::move(consoleSink));
            configureLogPipeline();
            configureSafaia();
            configureMcp();
            configureReloadAndStyle();
        }

        ~CoreServices() { shutdown(); }

        CoreServices(const CoreServices&)            = delete;
        CoreServices& operator=(const CoreServices&) = delete;

        bool mcpEnabled() const { return mcpConfig_.enabled; }

        void setConsoleSinkFanout(ConsoleSink sink) {
            std::lock_guard<std::mutex> lk(consoleSinkMutex_);
            consoleSink_ = std::move(sink);
            if (!consoleSink_) {
                consoleSink_ = defaultConsoleSink();
            }
        }

        void writeConsole(const std::string& line, ConsoleColor color) const {
            ConsoleSink sink;
            {
                std::lock_guard<std::mutex> lk(consoleSinkMutex_);
                sink = consoleSink_;
            }
            if (sink) {
                sink(line, color);
            }
        }

        void onGameProcessStarted(unsigned long pid) {
            styleProcessor.setPid(static_cast<int>(pid));
            if (mcp) {
                mcp->setMinecraftProcessId(static_cast<int>(pid));
            }
            if (safaia) {
                safaia->setMinecraftPid(static_cast<uint32_t>(pid));
                if (safaia->start()) {
                    writeConsole(
                        "[MCDK] Safaia 日志控制器已启动（端口 " + std::to_string(safaia->boundPort()) + "）",
                        ConsoleColor::Green
                    );
                } else {
                    writeConsole("[MCDK] Safaia 日志控制器启动失败，Minecraft 日志不可用", ConsoleColor::Yellow);
                }
            }
        }

        void startHotReloadIfNeeded(bool autoHotReload, const std::vector<UserModDirConfig>* modDirList, unsigned long pid) {
            if (!autoHotReload || modDirList == nullptr) {
                return;
            }
            reloadTask.setProcessId(static_cast<int>(pid));
            std::cout << "[HotReload] 追踪目录列表：\n";
            for (const auto& modDirConfig : *modDirList) {
                if (modDirConfig.hotReload) {
                    std::cout << "  └── " << modDirConfig.getAbsoluteU8String() << "\n";
                }
            }
            reloadTask.setModDirs(UserModDirConfig::toPathList(*modDirList));
            reloadTask.start();
        }

        void startStyleProcessor() { styleProcessor.start(); }

        void shutdown() {
            if (shutdown_) {
                return;
            }
            shutdown_ = true;

            if (safaia) {
                safaia->stop();
            }
            if (logPump) {
                logPump->stop();
            }
            if (gameLog) {
                gameLog->flush();
            }

            reloadTask.safeExit();
            if (ipc) {
                ipc->safeExit();
            }
            styleProcessor.safeExit();
            customTools.stop();
            if (mcp) {
                mcp->stop();
            }
        }

        std::shared_ptr<MCDevTool::Debug::DebugIPCServer>     ipc;
        std::shared_ptr<LogBuffer>                            logBuffer;
        std::shared_ptr<LogBuffer>                            errBuffer;
        std::unique_ptr<MCPServer>                            mcp;
        std::shared_ptr<MCDevTool::Safaia::SafaiaController>  safaia;
        std::unique_ptr<GameLogProcessor>                     gameLog;
        std::unique_ptr<AsyncLogPump>                         logPump;
        CustomToolManager                                     customTools;
        ReloadWatcherTask                                     reloadTask;
        UserStyleProcessor                                    styleProcessor;

        static ConsoleSink defaultConsoleSink() {
            return [](const std::string& msg, ConsoleColor color) { printColoredAtomic(msg, color); };
        }

        static void printColoredAtomic(const std::string& msg, ConsoleColor color) {
#ifdef _WIN32
            std::lock_guard<std::mutex> lk(consoleMutex());
            HANDLE                      hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

            if (hConsole == INVALID_HANDLE_VALUE) {
                std::cout << msg << _MCDEV_LOG_OUTPUT_ENDL;
                return;
            }

            CONSOLE_SCREEN_BUFFER_INFO info;
            if (!GetConsoleScreenBufferInfo(hConsole, &info)) {
                std::cout << msg << _MCDEV_LOG_OUTPUT_ENDL;
                return;
            }

            WORD attr = 0;

            switch (color) {
            case ConsoleColor::Green:
                attr = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
                break;
            case ConsoleColor::Red:
                attr = FOREGROUND_RED | FOREGROUND_INTENSITY;
                break;
            case ConsoleColor::Blue:
                attr = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
                break;
            case ConsoleColor::Yellow:
                attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
                break;
            case ConsoleColor::Cyan:
                attr = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
                break;
            case ConsoleColor::Magenta:
                attr = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
                break;
            case ConsoleColor::White:
                attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
                break;
            case ConsoleColor::Black:
                attr = 0;
                break;
            case ConsoleColor::Gray:
                attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
                break;
            case ConsoleColor::DarkGray:
                attr = FOREGROUND_INTENSITY;
                break;
            default:
                break;
            }

            if (color != ConsoleColor::Default) {
                SetConsoleTextAttribute(hConsole, attr);
            }

            std::cout << msg << _MCDEV_LOG_OUTPUT_ENDL;

            if (color == ConsoleColor::Default) {
                return;
            }
            SetConsoleTextAttribute(hConsole, info.wAttributes);
#else
            (void)color;
            std::cout << msg << _MCDEV_LOG_OUTPUT_ENDL;
#endif
        }

    private:
        static std::mutex& consoleMutex() {
            static std::mutex mutex;
            return mutex;
        }

        void configureLogPipeline() {
            gameLog = std::make_unique<GameLogProcessor>(
                filterPython_,
                logBuffer,
                errBuffer,
                [this](const std::string& line, ConsoleColor color) { writeConsole(line, color); },
                mcpConfig_.enabled
            );
            GameLogProcessor* gameLogPtr = gameLog.get();
            logPump = std::make_unique<AsyncLogPump>(
                [gameLogPtr](std::string_view payload) { gameLogPtr->consume(payload); }
            );
        }

        void configureSafaia() {
            AsyncLogPump* logPumpPtr = logPump.get();
            safaia->configure(safaiaSettings_.controller);
            safaia->setLogger([this](const std::string& level, const std::string& msg) {
                ConsoleColor color = (level == "error")  ? ConsoleColor::Red
                                     : (level == "warn") ? ConsoleColor::Yellow
                                                         : ConsoleColor::Cyan;
                writeConsole("[Safaia] " + msg, color);
            });
            safaia->setMessageHandler([logPumpPtr](std::string_view payload) { logPumpPtr->post(payload); });
        }

        void configureMcp() {
            if (!mcpConfig_.enabled) {
                return;
            }

            writeConsole(
                "[MCDK] MCP服务器已启用：" + mcpConfig_.serverIp + ":" + std::to_string(mcpConfig_.serverPort),
                ConsoleColor::Green
            );
            mcp->start();
            mcp->setLogBuffer(logBuffer);
            mcp->setErrBuffer(errBuffer);

            mcp->setCodeExecuteHandler(
                [ipcServer = ipc](const std::string& code, bool isClient, bool directReturn) -> nlohmann::json {
                    auto makeTextResult = [](bool isError, const std::string& text) -> nlohmann::json {
                        return nlohmann::json{
                            {"isError", isError},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}
                        };
                    };

                    if (ipcServer->getClientCount() == 0) {
                        return makeTextResult(
                            true,
                            "Code execution failed. The player may not be in the game or the target is unavailable."
                        );
                    }

                    if (!directReturn) {
                        bool success = ipcServer->sendMessage(
                            isClient ? 3 : 4,
                            code
                        );
                        if (!success) {
                            return makeTextResult(
                                true,
                                "Code execution failed. The player may not be in the game or the target is unavailable."
                            );
                        }
                        return makeTextResult(
                            false,
                            "Code executed successfully on the target side. Please use get_latest_logs to observe the "
                            "execution result."
                        );
                    }

                    nlohmann::json params = {
                        {"code", code},
                        {"is_client", isClient}
                    };
                    auto result = ipcServer->requestJson("execute_code", params.dump(), 10000);
                    if (!result.success) {
                        return makeTextResult(true, "Code execution failed: " + result.errorMessage);
                    }

                    auto response = nlohmann::json::parse(result.responseJson, nullptr, false);
                    if (response.is_discarded() || !response.is_object()) {
                        return makeTextResult(true, "Code execution returned invalid JSON: " + result.responseJson);
                    }
                    if (!response.value("ok", false)) {
                        std::string message = response.dump();
                        if (response.contains("error")) {
                            const auto& error = response["error"];
                            if (error.is_object() && error.contains("message")) {
                                message = error.value("message", message);
                            }
                        }
                        return makeTextResult(true, "Code execution failed: " + message);
                    }

                    nlohmann::json payload = nlohmann::json::object();
                    if (response.contains("result")) {
                        payload = response["result"];
                    }
                    std::ostringstream text;
                    text << "Code executed successfully on " << (isClient ? "client" : "server") << ".";
                    if (payload.is_object()) {
                        if (payload.contains("return_type")) {
                            text << "\nReturn type: " << payload.value("return_type", "unknown");
                        }
                        if (payload.contains("return_repr")) {
                            text << "\nReturn repr: " << payload.value("return_repr", "");
                        }
                        if (payload.contains("return_value")) {
                            text << "\nReturn value JSON: " << payload["return_value"].dump(2);
                        }
                    } else {
                        text << "\nReturn value JSON: " << payload.dump(2);
                    }
                    return makeTextResult(false, text.str());
                }
            );

            mcp->setReloadGameHandler([ipcServer = ipc]() -> bool {
                if (ipcServer->getClientCount() == 0) {
                    return false;
                }
                return ipcServer->sendMessage(5);
            });

            mcp->setReloadAddonAndGameHandler([ipcServer = ipc]() -> bool {
                if (ipcServer->getClientCount() == 0) {
                    return false;
                }
                return ipcServer->sendMessage(8);
            });

            mcp->setReloadShadersHandler([ipcServer = ipc]() -> bool {
                if (ipcServer->getClientCount() == 0) {
                    return false;
                }
                return ipcServer->sendMessage(6);
            });

            mcp->setReloadOnceShadersHandler([ipcServer = ipc](const std::string& fileName) -> bool {
                if (ipcServer->getClientCount() == 0) {
                    return false;
                }
                return ipcServer->sendMessage(7, fileName);
            });

            if (mcpConfig_.anyUiDebugEnabled() && safaiaSettings_.uiDebugEnabled) {
                mcp->setSafaiaController(safaia);
            }

            customTools.configure(mcp.get(), ipc);
            customTools.setIncludeUserTools(mcpConfig_.customTools);
            customTools.setLogger([this](const std::string& level, const std::string& msg) {
                ConsoleColor color = (level == "error")  ? ConsoleColor::Red
                                     : (level == "warn") ? ConsoleColor::Yellow
                                                         : ConsoleColor::Cyan;
                writeConsole("[CustomTools] " + msg, color);
            });
            customTools.start();

            if (mcpConfig_.customTools) {
                auto resyncTool = mcp::tool_builder("resync_custom_tools")
                                      .with_description(
                                          "Re-scan and re-sync custom MCP tools from the game (picks up added/removed "
                                          "@mcp_tool functions and edited tool bodies). Returns the current custom tool set. "
                                          "The stdio bridge forwards tools/list_changed automatically; reconnect only if "
                                          "the MCP client does not honor that notification."
                                      )
                                      .with_read_only_hint(false)
                                      .build();
                mcp->registerDynamicTool(
                    resyncTool,
                    [this](const nlohmann::json&, const std::string&) -> nlohmann::json {
                        customTools.syncNow();
                        auto           names = customTools.registeredNames();
                        nlohmann::json arr   = nlohmann::json::array();
                        for (const auto& n : names) {
                            arr.push_back(n);
                        }
                        std::string text = "Custom tools resynced. Registered (" + std::to_string(names.size())
                                         + "): " + arr.dump();
                        return nlohmann::json{
                            {"isError", false},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}
                        };
                    }
                );
            }
        }

        void configureReloadAndStyle() {
            reloadTask.setHotReloadAction([ipcServer = ipc](const nlohmann::json& targetPaths) {
                ipcServer->sendMessage(2, targetPaths.dump());
            });
            reloadTask.setOutputCallback([this](const std::string& msg, ConsoleColor color) {
                writeConsole(msg, color);
            });
            styleProcessor.setOutputCallback([this](const std::string& msg, ConsoleColor color) {
                writeConsole(msg, color);
            });
        }

    };

} // namespace mcdk
