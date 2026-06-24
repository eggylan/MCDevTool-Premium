// MCDK (no-GUI)
//
// 与 main 分支行为对齐的入口：建世界 → 创进程 → 阻塞等游戏退出。
// 走 P2-a 重构后的 CoreServices + GameLauncher 路径（决策 12：保留重构）。
// 不链 Qt、不识别 --gui/--no-gui（身份即模式）。

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <cwchar>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cstring>
#endif

#include "modules/config.hpp"
#include "modules/core_services.hpp"
#include "modules/env.hpp"
#include "modules/game_launcher.hpp"

#ifdef MCDK_ENABLE_CLI
#ifdef _WIN32
int MCDK_CLI_PARSE(int argc, wchar_t* argv[]);
#else
int MCDK_CLI_PARSE(int argc, char* argv[]);
#endif
#endif

namespace {

    int runNoGui(const nlohmann::json& config) {
        mcdk::CoreServices   core(config);
        mcdk::GameLauncher   launcher;
        auto                 prepared = launcher.prepareWorld(config);
        launcher.runBlocking(config, prepared, core);
        return 0;
    }

} // namespace

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
        // 决策 8：CLI 子模式（CLI11）默认 OFF；启用时任何参数都走 CLI 路径。
        // 决策 3：--gui/--no-gui 已删除，不再需要 hasNonModeArguments 过滤。
#ifdef MCDK_ENABLE_CLI
        if (argc > 1) {
            return MCDK_CLI_PARSE(argc, argv);
        }
#endif
        auto config = mcdk::userParseConfig();
        return runNoGui(config);
#ifdef NDEBUG
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }
#endif
    return 0;
}
