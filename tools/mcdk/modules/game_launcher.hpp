#pragma once

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <mcdevtool/addon.h>
#include <mcdevtool/env.h>
#include <mcdevtool/level.h>
#include <mcdevtool/utils.h>

#include "./config.hpp"
#include "./core_services.hpp"
#include "./env.hpp"
#include "./level.hpp"
#include "./mod_dir_config.hpp"
#include "./mod_register.hpp"
#include "./utils.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace mcdk {

    class GameLauncher {
    public:
        struct PrepareResult {
            std::filesystem::path          gameExePath;
            std::string                    launchConfig;
            std::vector<UserModDirConfig>  modDirConfigs;
            bool                           subprocessMode = false;
        };

        PrepareResult prepareWorld(const nlohmann::json& config) {
            auto gameExePath = std::filesystem::u8path(config.value("game_executable_path", ""));
            if (!std::filesystem::is_regular_file(gameExePath)) {
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

            PrepareResult result;
            result.gameExePath = std::move(gameExePath);

            result.subprocessMode = mcdk::getEnvIsSubprocessMode();
            if (!result.subprocessMode) {
                MCDevTool::cleanRuntimePacks();
            }

            result.modDirConfigs =
                UserModDirConfig::parseListFromJson(config.value("included_mod_dirs", nlohmann::json::array({"./"})));

            if (result.subprocessMode) {
                return result;
            }

            std::vector<MCDevTool::Addon::PackInfo> linkedPacks;
            if (config.value("include_debug_mod", true)) {
                auto debugMod = mcdk::registerDebugMod(config, result.modDirConfigs);
                std::cout << "[MCDK] 已注册调试MOD：" << debugMod.uuid << "\n";
                linkedPacks.push_back(std::move(debugMod));
                mcdk::linkUserConfigModDirs(result.modDirConfigs, linkedPacks);
            } else {
                mcdk::linkUserConfigModDirs(result.modDirConfigs, linkedPacks);
            }

            auto worldFolderName = config.value("world_folder_name", "MC_DEV_WORLD");
            auto resetWorld      = config.value("reset_world", false);
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
                if (mcdk::getEnvIsPluginEnv()) {
                    auto levelOptions = mcdk::parseLevelOptionsFromUserConfig(config);
                    MCDevTool::Level::updateLevelDatWorldDataInFile(worldsPath / "level.dat", std::nullopt, levelOptions);
                } else {
                    MCDevTool::Level::updateLevelDatLastPlayedInFile(worldsPath / "level.dat");
                }
            }

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
                autoJoinGame = (envAutoJoin == 1);
            }
            std::string targetBehJson = "netease_world_behavior_packs.json";
            std::string targetResJson = "netease_world_resource_packs.json";
            if (autoJoinGame) {
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
                return result;
            }

            auto configPath = worldsPath / "dev_config.cppconfig";
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
                MCDevTool::Utils::pathToGenericUtf8(result.gameExePath.parent_path() / "data/skin_packs/vanilla/steve.png");

            if (config.contains("skin_info") && config["skin_info"].is_object()) {
                devConfig["skin_info"] = config["skin_info"];
                auto& skinInfo         = devConfig["skin_info"];
                if (!skinInfo.contains("slim")) {
                    skinInfo["slim"] = false;
                }
                std::string skinPath = skinInfo.value("skin", "");
                if (skinPath.empty()) {
                    skinInfo["skin"] = defaultSkinPath;
                    skinPath         = std::move(defaultSkinPath);
                }
                if (!skinPath.empty()) {
                    auto fSkinPath = std::filesystem::u8path(skinPath);
                    if (!std::filesystem::is_regular_file(fSkinPath)) {
                        throw std::runtime_error("自定义皮肤文件不存在：" + skinPath);
                    }
                }
            } else {
                devConfig["skin_info"] = {{"slim", false}, {"skin", std::move(defaultSkinPath)}};
            }
            std::ofstream configFile(configPath);
            configFile << devConfig.dump(4);
            configFile.close();
            result.launchConfig = MCDevTool::Utils::pathToGenericUtf8(configPath);
            return result;
        }

#ifdef _WIN32
        DWORD spawn(const nlohmann::json& config, const PrepareResult& prepared, CoreServices& core) {
            if (hasProcess_) {
                throw std::runtime_error("Minecraft process is already running.");
            }

            bool autoHotReload = config.value("auto_hot_reload_mods", true);
            bool enableIPC     = autoHotReload;
            if (core.mcpEnabled()) {
                autoHotReload = true;
                enableIPC     = true;
            }

            std::wstring newEnv;
            void*        lpEnvironment = nullptr;
            if (enableIPC) {
                newEnv        = startIpcAndBuildEnvironment(config, core);
                lpEnvironment = static_cast<void*>(newEnv.data());
            }

            SECURITY_ATTRIBUTES sa{};
            sa.nLength              = sizeof(sa);
            sa.bInheritHandle       = TRUE;
            sa.lpSecurityDescriptor = nullptr;

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

            STARTUPINFOW si = {sizeof(si)};
            si.dwFlags     |= STARTF_USESTDHANDLES;
            si.hStdOutput   = hNul;
            si.hStdError    = hNul;
            si.hStdInput    = GetStdHandle(STD_INPUT_HANDLE);

            auto        neteaseConfig = config.value("netease_config", nlohmann::json::object());
            std::string cmd           = "\"" + MCDevTool::Utils::pathToUtf8(prepared.gameExePath) + "\"";
            if (!neteaseConfig.value("chat_extension", false)) {
                cmd.append(" chatExtension=false");
            }

            if (!prepared.launchConfig.empty()) {
                cmd.append(" config=\"" + prepared.launchConfig + "\"");
            }

            auto        ptvsdConfig = mcdk::getPtvsdConfigFromJson(config);
            std::string ptvsdArgs   = mcdk::buildPtvsdLaunchArgs(ptvsdConfig);
            if (!ptvsdArgs.empty()) {
                cmd.append(" " + ptvsdArgs);
                core.writeConsole(
                    "[MCDK] ptvsd 调试已启用: " + ptvsdConfig.ip + ":" + std::to_string(ptvsdConfig.port),
                    ConsoleColor::Cyan
                );
            }

            std::wstring wideCmd = convertUtf8ToUtf16(cmd);
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(
                    nullptr,
                    wideCmd.data(),
                    nullptr,
                    nullptr,
                    TRUE,
                    (lpEnvironment != nullptr ? CREATE_UNICODE_ENVIRONMENT : 0),
                    lpEnvironment,
                    nullptr,
                    &si,
                    &pi
                )) {
                CloseHandle(hNul);
                throw std::runtime_error("CreateProcessW failed");
            }

            CloseHandle(hNul);

            currentProcess_ = pi;
            hasProcess_     = true;
            DWORD pid       = pi.dwProcessId;

            core.onGameProcessStarted(pid);
            attachLegacyDebuggerIfNeeded(config, pid);
            core.startHotReloadIfNeeded(autoHotReload, &prepared.modDirConfigs, pid);
            core.startStyleProcessor();

            return pid;
        }

        void runBlocking(const nlohmann::json& config, const PrepareResult& prepared, CoreServices& core) {
            spawn(config, prepared, core);
            WaitForSingleObject(currentProcess_.hProcess, INFINITE);
            core.shutdown();
            closeProcessHandles();
        }

        void stop() {
            if (!hasProcess_) {
                return;
            }
            TerminateProcess(currentProcess_.hProcess, 1);
        }

        bool isRunning() const { return hasProcess_; }
#else
        unsigned long spawn(const nlohmann::json&, const PrepareResult&, CoreServices&) {
            throw std::runtime_error("mcdk game launch is only supported on Windows.");
        }

        void runBlocking(const nlohmann::json&, const PrepareResult&, CoreServices&) {
            throw std::runtime_error("mcdk game launch is only supported on Windows.");
        }

        void stop() {}
        bool isRunning() const { return false; }
#endif

    private:
#ifdef _WIN32
        static void attachLegacyDebuggerIfNeeded(const nlohmann::json& userConfig, DWORD pid) {
            int debuggerPort = mcdk::getEnvDebuggerPort();
            if (debuggerPort == 0) {
                auto debuggerConfig = userConfig.value("modpc_debugger", nlohmann::json::object());
                if (debuggerConfig.is_object()) {
                    bool debuggerEnabled = debuggerConfig.value("enabled", false);
                    if (debuggerEnabled) {
                        debuggerPort = debuggerConfig.value("port", 5632);
                    }
                }
            }

            if (debuggerPort <= 0) {
                return;
            }

            std::string cmd;
            cmd.reserve(48);
            cmd.append("mcdbg.exe --pid ");
            cmd.append(std::to_string(pid));
            cmd.append(" --port ");
            cmd.append(std::to_string(debuggerPort));

            STARTUPINFOA        si = {sizeof(si)};
            PROCESS_INFORMATION pi = {};
            if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                std::cerr << "警告：无法启动mcdbg.exe附加调试器，请确保其在环境变量路径中。" << _MCDEV_LOG_OUTPUT_ENDL;
                return;
            }
            std::cout << "调试器已启动，正在附加到进程PID：" << pid << " 端口：" << debuggerPort << " ..."
                      << _MCDEV_LOG_OUTPUT_ENDL;
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }

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

        static std::wstring createNewEnvironmentBlock(
            const std::vector<std::pair<std::wstring, std::wstring>>& newVars
        ) {
            LPWCH envBlock = GetEnvironmentStringsW();
            if (envBlock == nullptr) {
                throw std::runtime_error("Failed to get current environment strings.");
            }

            std::wstring newEnvBlock;
            LPWCH        current = envBlock;
            while (*current) {
                std::wstring varLine(current);
                newEnvBlock += varLine + L'\0';
                current     += varLine.size() + 1;
            }

            for (const auto& [name, value] : newVars) {
                newEnvBlock += name + L'=' + value + L'\0';
            }

            newEnvBlock += L'\0';
            FreeEnvironmentStringsW(envBlock);
            return newEnvBlock;
        }

        static std::wstring startIpcAndBuildEnvironment(const nlohmann::json& userConfig, CoreServices& core) {
            core.ipc->start();
            int port = core.ipc->getPort();
            core.writeConsole("[MCDK] IPC调试服务器已启动，端口：" + std::to_string(port), ConsoleColor::Green);

            std::vector<std::pair<std::wstring, std::wstring>> envVars;
            envVars.emplace_back(L"MCDEV_DEBUG_IPC_PORT", std::to_wstring(port));

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
                std::filesystem::create_directories(globalToolsDir, ec);
                envVars.emplace_back(L"MCDEV_GLOBAL_MCP_TOOLS", globalToolsDir.wstring());
                auto        u8 = globalToolsDir.u8string();
                std::string shown(u8.begin(), u8.end());
                core.writeConsole("[MCDK] 全局自定义工具目录：" + shown, ConsoleColor::Green);
            }

            return createNewEnvironmentBlock(envVars);
        }

        void closeProcessHandles() {
            if (!hasProcess_) {
                return;
            }
            CloseHandle(currentProcess_.hProcess);
            CloseHandle(currentProcess_.hThread);
            currentProcess_ = {};
            hasProcess_     = false;
        }

        PROCESS_INFORMATION currentProcess_{};
        bool                hasProcess_ = false;
#endif
    };

} // namespace mcdk
