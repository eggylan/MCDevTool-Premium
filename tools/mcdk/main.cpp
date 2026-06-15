// MCDK
#include <algorithm>
#include <sstream>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>


// mcdk modules
#include "modules/config.hpp"
#include "modules/console.hpp"
#include "modules/env.hpp"
#include "modules/hotreload.hpp"
#include "modules/level.hpp"
#include "modules/mod_dir_config.hpp"
#include "modules/mod_register.hpp"
#include "modules/style_processor.hpp"
#include "modules/utils.hpp"
#include "modules/log_buffer.hpp"
#include "modules/mcp_server.hpp"
#include "modules/custom_tool_manager.hpp"
#include "modules/game_log_processor.hpp"
#include "modules/async_log_pump.hpp"


// mcdevtool api
#include <mcdevtool/addon.h>
#include <mcdevtool/utils.h>
#include <mcdevtool/debug.h>
#include <mcdevtool/env.h>
#include <mcdevtool/level.h>
#include <mcdevtool/style.h>
#include <nlohmann/json.hpp>


// 默认使用"\n"而非std::endl输出日志，避免大量log的性能开销
#define _MCDEV_LOG_OUTPUT_ENDL "\n"

#ifdef MCDEV_LOG_FORCE_FLUSH_ENDL
// 强制使用std::endl
#undef _MCDEV_LOG_OUTPUT_ENDL
#define _MCDEV_LOG_OUTPUT_ENDL std::endl
#endif

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using mcdk::ReloadWatcherTask;
using mcdk::UserModDirConfig;
using mcdk::UserStyleProcessor;
using ConsoleColor = mcdk::ConsoleColor;

static std::mutex g_consoleMutex;

// 线程安全彩色输出
static void printColoredAtomic(const std::string& msg, ConsoleColor color) {
    std::lock_guard<std::mutex> lk(g_consoleMutex);
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
        // 亮灰 = RGB，但不加高亮
        attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        break;
    case ConsoleColor::DarkGray:
        // 深灰 = 只加高亮，不加 RGB
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
    // 恢复原色
    SetConsoleTextAttribute(hConsole, info.wAttributes);
}

// 进程buffer行处理
#ifdef _WIN32

// 尝试附加调试器到指定进程
static void debuggerAttachToProcess(DWORD pid, int port) {
    // 执行cmd调用mcdbg.exe附加（如果失败则输出错误信息）
    std::string cmd;
    cmd.reserve(48);

    cmd.append("mcdbg.exe --pid ");
    cmd.append(std::to_string(pid));
    cmd.append(" --port ");
    cmd.append(std::to_string(port));
    STARTUPINFOA        si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        std::cerr << "警告：无法启动mcdbg.exe附加调试器，请确保其在环境变量路径中。" << _MCDEV_LOG_OUTPUT_ENDL;
        return;
    }
    std::cout << "调试器已启动，正在附加到进程PID：" << pid << " 端口：" << port << " ..." << _MCDEV_LOG_OUTPUT_ENDL;
}

// 检查用户配置是否需要启用IPC调试功能
static bool checkUserConfigEnableIPC(const nlohmann::json& userConfig) {
    // 若启用了auto_hot_reload_mods则启用IPC
    bool autoHotReload = userConfig.value("auto_hot_reload_mods", true);
    if (autoHotReload) {
        return true;
    }
    return false;
}

// 将utf8的string转换为utf16的wstring
static std::wstring convertUtf8ToUtf16(const std::string& utf8Str) {
    if (utf8Str.empty()) {
        return std::wstring();
    }
    int wideCharLen = MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), static_cast<int>(utf8Str.size()), nullptr, 0);
    if (wideCharLen == 0) {
        throw std::runtime_error("Failed to convert UTF-8 to UTF-16.");
    }
    std::wstring utf16Str(wideCharLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), static_cast<int>(utf8Str.size()), &utf16Str[0], wideCharLen);
    return utf16Str;
}

// 生成新的环境变量w字符串（继承当前环境变量并添加新变量）
static std::wstring createNewEnvironmentBlock(
    const std::vector<std::pair<std::wstring, std::wstring>>& newVars
) {
    // 获取当前环境变量块
    LPWCH envBlock = GetEnvironmentStringsW();
    if (envBlock == nullptr) {
        throw std::runtime_error("Failed to get current environment strings.");
    }

    std::wstring newEnvBlock;
    // 复制现有环境变量
    LPWCH current = envBlock;
    while (*current) {
        std::wstring varLine(current);
        newEnvBlock += varLine + L'\0';
        current     += varLine.size() + 1;
    }

    // 追加新的环境变量
    for (const auto& [name, value] : newVars) {
        newEnvBlock += name + L'=' + value + L'\0';
    }

    // 结束环境变量块
    newEnvBlock += L'\0';

    FreeEnvironmentStringsW(envBlock);
    return newEnvBlock;
}

// 启动游戏可执行文件
static void launchGameExe(
    const std::filesystem::path&         exePath,
    std::string_view                     config     = "",
    const nlohmann::json&                userConfig = nlohmann::json::object(),
    const std::vector<UserModDirConfig>* modDirList = nullptr
) {
    bool  autoHotReload = userConfig.value("auto_hot_reload_mods", true);
    bool  enableIPC     = autoHotReload;
    void* lpEnvironment = nullptr;

    auto mcpServerConfig = mcdk::getMcpServerConfigFromJson(userConfig);
    auto safaiaSettings  = mcdk::getSafaiaUserSettingsFromJson(userConfig);
    bool filterPython    = userConfig.value("include_debug_mod", true);

    auto ipcServer       = MCDevTool::Debug::createDebugServer();
    auto logBuffer       = std::make_shared<mcdk::LogBuffer>(1000, 250);
    auto errBuffer       = std::make_shared<mcdk::LogBuffer>(1000, 400);
    auto mcpServer       = mcdk::MCPServer(mcpServerConfig);
    auto safaiaController = std::make_shared<MCDevTool::Safaia::SafaiaController>();
    mcdk::CustomToolManager customToolManager;

    // 游戏日志处理器：消费 Safaia 协议 4 的原始日志。控制台始终输出；仅 MCP 启用时
    // 写入 log/err 缓冲(供 MCP 日志查询工具)。必须在注册 Safaia message handler 之前创建，
    // 并存活到 controller 完全停止之后(见函数末尾 stop -> flush 顺序)。
    auto gameLog = std::make_unique<mcdk::GameLogProcessor>(
        filterPython,
        logBuffer,
        errBuffer,
        [](const std::string& line, ConsoleColor color) { printColoredAtomic(line, color); },
        mcpServerConfig.enabled // enableBuffering：MCP 关闭时不缓冲，但控制台仍输出
    );
    mcdk::GameLogProcessor* gameLogPtr = gameLog.get();

    // 异步日志泵：Safaia recv 线程只入队原始 payload(post)，由 worker 线程调用 consume 做实际处理。
    // 这样控制台/缓冲变慢也不会阻塞 recv 线程，避免反压游戏侧 Safaia 发送导致界面卡死。
    auto logPump = std::make_unique<mcdk::AsyncLogPump>(
        [gameLogPtr](std::string_view payload) { gameLogPtr->consume(payload); }
    );
    mcdk::AsyncLogPump* logPumpPtr = logPump.get();

    // Safaia 控制器是日志基础组件：每次启动 Minecraft 都配置并(在拿到 PID 后)启动，
    // 不依赖 mcp_server_config.enabled / ui_* / safaia_ui_debug.enabled。
    safaiaController->configure(safaiaSettings.controller);
    safaiaController->setLogger([](const std::string& level, const std::string& msg) {
        ConsoleColor color = (level == "error")  ? ConsoleColor::Red
                             : (level == "warn") ? ConsoleColor::Yellow
                                                 : ConsoleColor::Cyan;
        printColoredAtomic("[Safaia] " + msg, color);
    });
    // recv 线程仅入队，绝不在此做可能阻塞的处理(见 AsyncLogPump 注释)。
    safaiaController->setMessageHandler([logPumpPtr](std::string_view payload) { logPumpPtr->post(payload); });

    if (mcpServerConfig.enabled) {
        // 若启用MCP服务器将自动启用IPC调试功能
        autoHotReload = true;
        enableIPC     = true;
        printColoredAtomic(
            "[MCDK] MCP服务器已启用：" + mcpServerConfig.serverIp + ":" + std::to_string(mcpServerConfig.serverPort),
            ConsoleColor::Green
        );
        mcpServer.start();
        mcpServer.setLogBuffer(logBuffer);
        mcpServer.setErrBuffer(errBuffer);

        // 代码执行Handler
        mcpServer.setCodeExecuteHandler(
            [ipcServer](const std::string& code, bool isClient, bool directReturn) -> nlohmann::json {
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
                        isClient ? 3 : 4, // 3 = CLIENT_CODE_EXECUTE, 4 = SERVER_CODE_EXECUTE
                        code
                    ); // CODE EXECUTE
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

        // 重载游戏
        mcpServer.setReloadGameHandler([ipcServer]() -> bool {
            if (ipcServer->getClientCount() == 0) {
                return false; // 没有连接的客户端，无法执行
            }
            return ipcServer->sendMessage(5); // GAME RELOAD
        });

        // 重载插件和游戏
        mcpServer.setReloadAddonAndGameHandler([ipcServer]() -> bool {
            if (ipcServer->getClientCount() == 0) {
                return false; // 没有连接的客户端，无法执行
            }
            return ipcServer->sendMessage(8); // ADDON AND GAME RELOAD
        });

        // 重载着色器（重新编译着色器）
        mcpServer.setReloadShadersHandler([ipcServer]() -> bool {
            if (ipcServer->getClientCount() == 0) {
                return false; // 没有连接的客户端，无法执行
            }
            return ipcServer->sendMessage(6); // SHADERS RELOAD
        });

        // 重载单个着色器（重新编译单个着色器）
        mcpServer.setReloadOnceShadersHandler([ipcServer](const std::string& fileName) -> bool {
            if (ipcServer->getClientCount() == 0) {
                return false; // 没有连接的客户端，无法执行
            }
            return ipcServer->sendMessage(7, fileName); // ONCE SHADER RELOAD
        });

        // ── Safaia UI 调试：仅把控制器暴露给 MCP 的 ui_* 工具 ──
        // 控制器本身已在 MCP 块外配置并随 Minecraft 启动(日志基础组件)。这里只决定是否
        // 注入给 UI 工具：需 MCP 启用 + 至少一个 ui_* 工具启用 + safaia_ui_debug.enabled。
        // 不满足时控制器仍运行并接收日志，仅 UI 调试能力不可用。
        if (mcpServerConfig.anyUiDebugEnabled() && safaiaSettings.uiDebugEnabled) {
            mcpServer.setSafaiaController(safaiaController);
        }

        // ── 自定义 MCP 工具管理器 ──
        // 始终启动：注册内置 health_check(始终启用,不可关);用户 @mcp_tool 工具及 resync_custom_tools
        // 受 mcp_server_config.tools.custom_tools 开关约束。
        customToolManager.configure(&mcpServer, ipcServer);
        customToolManager.setIncludeUserTools(mcpServerConfig.customTools);
        customToolManager.setLogger([](const std::string& level, const std::string& msg) {
            ConsoleColor color = (level == "error")  ? ConsoleColor::Red
                                 : (level == "warn") ? ConsoleColor::Yellow
                                                     : ConsoleColor::Cyan;
            printColoredAtomic("[CustomTools] " + msg, color);
        });
        customToolManager.start();

        if (mcpServerConfig.customTools) {
            // 用户可手动触发的自定义工具重扫/重同步：重新拉取游戏侧注册表(含重扫 mcp_tools/ 目录)，
            // 动态增删一等 MCP 工具并广播 tools/list_changed。
            auto resyncTool = mcp::tool_builder("resync_custom_tools")
                                  .with_description(
                                      "Re-scan and re-sync custom MCP tools from the game (picks up added/removed "
                                      "@mcp_tool functions and edited tool bodies). Returns the current custom tool set. "
                                      "The stdio bridge forwards tools/list_changed automatically; reconnect only if "
                                      "the MCP client does not honor that notification."
                                  )
                                  .with_read_only_hint(false)
                                  .build();
            mcpServer.registerDynamicTool(
                resyncTool,
                [&customToolManager](const nlohmann::json&, const std::string&) -> nlohmann::json {
                    customToolManager.syncNow();
                    auto           names = customToolManager.registeredNames();
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
    mcdk::ReloadWatcherTask  reloadTask;
    mcdk::UserStyleProcessor styleProcessor(0, userConfig);
    reloadTask.setHotReloadAction([ipcServer](const nlohmann::json& targetPaths) {
        ipcServer->sendMessage(2, targetPaths.dump()); // FAST RELOAD
    });
    // reloadTask.bindServer(ipcServer);
    reloadTask.setOutputCallback(printColoredAtomic);
    styleProcessor.setOutputCallback(printColoredAtomic);

    std::wstring newEnv;
    if (enableIPC) {
        ipcServer->start();
        int port = ipcServer->getPort();
        // std::cout << "[MCDK] IPC调试服务器已启动，端口：" << port <<
        // _MCDEV_LOG_OUTPUT_ENDL;
        printColoredAtomic("[MCDK] IPC调试服务器已启动，端口：" + std::to_string(port), ConsoleColor::Green);

        std::vector<std::pair<std::wstring, std::wstring>> envVars;
        envVars.emplace_back(L"MCDEV_DEBUG_IPC_PORT", std::to_wstring(port));

        // 全局自定义工具目录：默认 %USERPROFILE%\.mcdk\mcp_tools，可由 .mcdev.json 的
        // global_mcp_tools_dir 覆盖（显式空串则禁用）。所有项目共享，开发者放一次处处可用。
        std::filesystem::path globalToolsDir;
        if (userConfig.contains("global_mcp_tools_dir") && userConfig["global_mcp_tools_dir"].is_string()) {
            std::string v = userConfig["global_mcp_tools_dir"].get<std::string>();
            if (!v.empty()) {
                globalToolsDir = std::filesystem::path(v);
            }
        } else if (const wchar_t* up = _wgetenv(L"USERPROFILE"); up && *up) {
            globalToolsDir = std::filesystem::path(up) / L".mcdk" / L"mcp_tools";
        }
        if (!globalToolsDir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(globalToolsDir, ec); // best-effort，便于开发者找到投放位置
            envVars.emplace_back(L"MCDEV_GLOBAL_MCP_TOOLS", globalToolsDir.wstring());
            auto        u8 = globalToolsDir.u8string();
            std::string shown(u8.begin(), u8.end());
            printColoredAtomic("[MCDK] 全局自定义工具目录：" + shown, ConsoleColor::Green);
        }

        newEnv        = createNewEnvironmentBlock(envVars);
        lpEnvironment = (void*)newEnv.data();
    }

    STARTUPINFOW        si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};

    SECURITY_ATTRIBUTES sa{};
    sa.nLength              = sizeof(sa);
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    // 日志统一经 Safaia 协议 4 获取，不再使用 stdout/stderr 匿名管道。
    // 将子进程 stdout/stderr 重定向到 NUL，避免继承控制台或无人读取的管道造成重复输出 / 缓冲区阻塞。
    HANDLE hNul = CreateFileW(
        L"NUL",
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        OPEN_EXISTING,
        0,
        nullptr
    );
    if (hNul == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("CreateFileW(NUL) failed");
    }

    si.dwFlags    |= STARTF_USESTDHANDLES;
    si.hStdOutput  = hNul;
    si.hStdError   = hNul;
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);

    auto neteaseConfig = userConfig.value("netease_config", nlohmann::json::object());

    // Build command
    std::string cmd = "\"" + MCDevTool::Utils::pathToUtf8(exePath) + "\"";
    if (!neteaseConfig.value("chat_extension", false)) {
        cmd.append(" chatExtension=false");
    }

    // 自定义config启动参数
    if (!config.empty()) {
        cmd.append(" config=\"" + std::string(config) + "\"");
    }

    // ptvsd 调试参数（官方调试器接口）
    auto        ptvsdConfig = mcdk::getPtvsdConfigFromJson(userConfig);
    std::string ptvsdArgs   = mcdk::buildPtvsdLaunchArgs(ptvsdConfig);
    if (!ptvsdArgs.empty()) {
        cmd.append(" " + ptvsdArgs);
        printColoredAtomic(
            "[MCDK] ptvsd 调试已启用: " + ptvsdConfig.ip + ":" + std::to_string(ptvsdConfig.port),
            ConsoleColor::Cyan
        );
    }

    if (!CreateProcessW(
            nullptr,
            convertUtf8ToUtf16(cmd).data(),
            nullptr,
            nullptr,
            TRUE, // 继承句柄
            (lpEnvironment != nullptr ? CREATE_UNICODE_ENVIRONMENT : 0),
            lpEnvironment,
            nullptr,
            &si,
            &pi
        )) {
        CloseHandle(hNul);
        throw std::runtime_error("CreateProcessA failed");
    }

    DWORD pid = pi.dwProcessId;
    // 设置样式处理器PID
    styleProcessor.setPid(pid);
    mcpServer.setMinecraftProcessId(pid);

    // 父进程不需要 NUL 句柄
    CloseHandle(hNul);

    // ── 启动 Safaia 控制器：拿到 PID 后再启动 TCP server + PID 定向 discovery ──
    // 控制器是日志基础组件，与 MCP/UI 无关；失败只警告，不阻止游戏与其他功能。
    safaiaController->setMinecraftPid(pid);
    if (safaiaController->start()) {
        printColoredAtomic(
            "[MCDK] Safaia 日志控制器已启动（端口 " + std::to_string(safaiaController->boundPort()) + "）",
            ConsoleColor::Green
        );
    } else {
        printColoredAtomic("[MCDK] Safaia 日志控制器启动失败，Minecraft 日志不可用", ConsoleColor::Yellow);
    }

    // ===================== 用户配置后置处理 =====================
    // 旧版 mcdbg 调试器端口（0为不启用；modpc_debugger 为历史兼容方案，见 README）
    int debuggerPort = mcdk::getEnvDebuggerPort();
    if (debuggerPort == 0) {
        // 解析用户配置覆盖
        auto debuggerConfig = userConfig.value("modpc_debugger", nlohmann::json::object());
        if (debuggerConfig.is_object()) {
            bool debuggerEnabled = debuggerConfig.value("enabled", false);
            if (debuggerEnabled) {
                debuggerPort = debuggerConfig.value("port", 5632);
            }
        }
    }

    if (debuggerPort > 0) {
        // 尝试启动mcdbg调试器附加（在官方调试器之前的历史产物）
        debuggerAttachToProcess(pid, debuggerPort);
    }

    if (autoHotReload && modDirList != nullptr) {
        // 启动热更新追踪任务
        reloadTask.setProcessId(pid);
        // 输出追踪目录列表
        std::cout << "[HotReload] 追踪目录列表：\n";
        for (const auto& modDirConfig : *modDirList) {
            if (modDirConfig.hotReload) {
                std::cout << "  └── " << modDirConfig.getAbsoluteU8String() << "\n";
            }
        }
        reloadTask.setModDirs(UserModDirConfig::toPathList(*modDirList));
        reloadTask.start();
    }
    styleProcessor.start();

    // 等待子进程退出
    WaitForSingleObject(pi.hProcess, INFINITE);

    // 先停止 Safaia 控制器，确保其读线程不再 post 新日志，再排空异步泵、flush 残行。
    safaiaController->stop();
    // 处理完队列中已入队的剩余日志后退出 worker。
    logPump->stop();
    // 处理最后一个无换行残行（断连/退出时）。
    gameLog->flush();

    // 停止热更新任务
    reloadTask.safeExit();
    // 停止IPC服务器 如果已启用
    ipcServer->safeExit();
    // 停止样式处理器
    styleProcessor.safeExit();
    // 停止自定义工具管理器（如已启用）
    customToolManager.stop();
    // 安全的关闭MCP服务器(如果已启用)
    mcpServer.stop();

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

#endif

// ===================== 启动游戏逻辑 =====================

// 启动游戏
static void startGame(const nlohmann::json& config) {
    auto gameExePath = std::filesystem::u8path(config.value("game_executable_path", ""));
    if (!std::filesystem::is_regular_file(gameExePath)) {
        // 游戏exe路径无效 重新搜索新版
        if (mcdk::updateGamePath(gameExePath)) {
            std::cout << "游戏路径无效，重新搜索：" << MCDevTool::Utils::pathToGenericUtf8(gameExePath) << "\n";
            std::string u8input;
            std::cout << "是否更新配置文件中的游戏路径？(y/n)：";
            std::getline(std::cin, u8input);
            if (u8input == "Y" || u8input == "y") {
                mcdk::tryUpdateUserGamePath(gameExePath);
                std::cout << "已更新配置文件中的游戏路径。\n";
            } else {
                throw std::runtime_error("未更新配置文件中的游戏路径，启动终止。");
            }
        } else {
            throw std::runtime_error("未能找到有效的游戏exe文件。");
        }
    }

    auto _isSubprocessMode = mcdk::getEnvIsSubprocessMode();

    if (!_isSubprocessMode) {
        MCDevTool::cleanRuntimePacks();
    }

    auto modDirConfigs =
        UserModDirConfig::parseListFromJson(config.value("included_mod_dirs", nlohmann::json::array({"./"})));

    if (_isSubprocessMode) {
        // 子进程模式 直接启动游戏exe（通常由vsc插件多开使用）
        launchGameExe(gameExePath, "", config, &modDirConfigs);
        return;
    }
    std::vector<MCDevTool::Addon::PackInfo> linkedPacks;
    if (config.value("include_debug_mod", true)) {
        auto debugMod = mcdk::registerDebugMod(config, modDirConfigs);
        std::cout << "[MCDK] 已注册调试MOD：" << debugMod.uuid << "\n";
        linkedPacks.push_back(std::move(debugMod));
        mcdk::linkUserConfigModDirs(modDirConfigs, linkedPacks);
    } else {
        mcdk::linkUserConfigModDirs(modDirConfigs, linkedPacks);
    }

    // 创建世界
    auto worldFolderName = config.value("world_folder_name", "MC_DEV_WORLD");
    auto resetWorld      = config.value("reset_world", false); // 若启用该参数 每次都会强制覆盖世界
    auto worldsPath      = MCDevTool::getMinecraftWorldsPath() / std::filesystem::u8path(worldFolderName);
    if (!std::filesystem::is_directory(worldsPath) || resetWorld) {
        std::filesystem::remove_all(worldsPath);
        if (resetWorld) {
            std::cout << "已删除旧世界数据，正在创建新世界...\n";
        }
        std::filesystem::create_directories(worldsPath);
        std::ofstream levelFile(worldsPath / "level.dat", std::ios::binary);
        auto          levelDat = mcdk::createUserLevel(config);
        levelFile.write(reinterpret_cast<const char*>(levelDat.data()), levelDat.size());
        levelFile.close();
    } else {
        // 更新level.dat的配置数据
        if (mcdk::getEnvIsPluginEnv()) {
            // 插件环境每次启动都要覆盖配置
            auto levelOptions = mcdk::parseLevelOptionsFromUserConfig(config);
            MCDevTool::Level::updateLevelDatWorldDataInFile(worldsPath / "level.dat", std::nullopt, levelOptions);
        } else {
            // 非插件环境只更新时间戳
            MCDevTool::Level::updateLevelDatLastPlayedInFile(worldsPath / "level.dat");
        }
    }

    // netease_world_behavior_packs.json / netease_world_resource_packs.json
    auto behPacksManifest = nlohmann::json::array();
    auto resPacksManifest = nlohmann::json::array();
    for (const auto& pack : linkedPacks) {
        nlohmann::json packEntry{
            {"pack_id", pack.uuid},
            {"version", nlohmann::json::parse(pack.version)},
        };
        if (pack.type == MCDevTool::Addon::PackType::BEHAVIOR) {
            behPacksManifest.push_back(std::move(packEntry));
        } else if (pack.type == MCDevTool::Addon::PackType::RESOURCE) {
            resPacksManifest.push_back(std::move(packEntry));
        }
    }

    auto autoJoinGame = config.value("auto_join_game", true);
    auto envAutoJoin  = mcdk::getEnvAutoJoinGameState();
    if (envAutoJoin != -1) {
        // 环境变量覆写配置文件
        autoJoinGame = (envAutoJoin == 1);
    }
    std::string targetBehJson = "netease_world_behavior_packs.json";
    std::string targetResJson = "netease_world_resource_packs.json";
    if (autoJoinGame) {
        // 使用国际版标准协议 避免网易串改
        targetBehJson = "world_behavior_packs.json";
        targetResJson = "world_resource_packs.json";
    }
    std::ofstream behManifestFile(worldsPath / targetBehJson);
    behManifestFile << behPacksManifest.dump(4);
    behManifestFile.close();

    std::ofstream resManifestFile(worldsPath / targetResJson);
    resManifestFile << resPacksManifest.dump(4);
    resManifestFile.close();

    if (!autoJoinGame) {
        // 不自动进入游戏 直接启动游戏exe
        launchGameExe(gameExePath, "", config, &modDirConfigs);
        return;
    }

    auto configPath = worldsPath / "dev_config.cppconfig";
    // 创建dev_config
    nlohmann::json devConfig{
        {"world_info", {{"level_id", worldFolderName}}},
        {"room_info", nlohmann::json::object()},
        {"player_info",
         {
             {"urs", ""},
             {"user_id", 0},
             {"user_name", config.value("user_name", "developer")},
         }},
    };

    auto defaultSkinPath =
        MCDevTool::Utils::pathToGenericUtf8(gameExePath.parent_path() / "data/skin_packs/vanilla/steve.png");

    if (config.contains("skin_info") && config["skin_info"].is_object()) {
        // 用户自定义skin_info
        devConfig["skin_info"] = config["skin_info"];
        auto& skinInfo         = devConfig["skin_info"];
        // 自动生成缺失字段
        if (!skinInfo.contains("slim")) {
            skinInfo["slim"] = false;
        }
        std::string skinPath = skinInfo.value("skin", "");
        if (skinPath.empty()) {
            skinInfo["skin"] = defaultSkinPath;
            skinPath         = std::move(defaultSkinPath);
        }
        // 安全校验 检查文件是否存在
        if (!skinPath.empty()) {
            auto fSkinPath = std::filesystem::u8path(skinPath);
            if (!std::filesystem::is_regular_file(fSkinPath)) {
                throw std::runtime_error("自定义皮肤文件不存在：" + skinPath);
            }
        }
    } else {
        // 自动生成skin_info
        devConfig["skin_info"] = {{"slim", false}, {"skin", std::move(defaultSkinPath)}};
    }
    std::ofstream configFile(configPath);
    configFile << devConfig.dump(4);
    configFile.close();
    launchGameExe(gameExePath, MCDevTool::Utils::pathToGenericUtf8(configPath), config, &modDirConfigs);
}

#ifdef MCDK_ENABLE_CLI
#ifdef _WIN32
int MCDK_CLI_PARSE(int argc, wchar_t* argv[]);
#else
int MCDK_CLI_PARSE(int argc, char* argv[]);
#endif
#endif

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#else
int main(int argc, char* argv[]) {
#endif

    if (mcdk::getEnvOutputMode() == 1) {
        setvbuf(stdout, nullptr, _IONBF, 0);
    }

#ifdef NDEBUG
    try {
#endif
#ifdef MCDK_ENABLE_CLI
        if (argc > 1) {
            return MCDK_CLI_PARSE(argc, argv);
        }
#endif
        auto config = mcdk::userParseConfig();
        startGame(config);
#ifdef NDEBUG
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }
#endif
    return 0;
}
