// MCDK-GUI (Qt GUI)
//
// GUI 入口：QApplication + 主窗，Core 常驻，用户点「启动游戏」受管启停。
// 链 Qt Widgets、定义 MCDK_ENABLE_GUI 宏（由 CMake 在 mcdk_gui target 上注入）。
// 不识别 --gui/--no-gui（身份即模式）。

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <cwchar>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cstring>
#endif

#include "modules/config.hpp"
#include "modules/env.hpp"
#include "modules/gui/app.hpp"

#ifdef MCDK_ENABLE_CLI
#ifdef _WIN32
int MCDK_CLI_PARSE(int argc, wchar_t* argv[]);
#else
int MCDK_CLI_PARSE(int argc, char* argv[]);
#endif
#endif

namespace {

#ifdef _WIN32
    using NativeArg = wchar_t*;

    std::string nativeArgToUtf8(const wchar_t* arg) {
        if (arg == nullptr || *arg == L'\0') {
            return {};
        }
        int required = WideCharToMultiByte(CP_UTF8, 0, arg, -1, nullptr, 0, nullptr, nullptr);
        if (required <= 0) {
            throw std::runtime_error("Failed to convert command line argument to UTF-8.");
        }
        std::string out(static_cast<std::size_t>(required), '\0');
        int written = WideCharToMultiByte(CP_UTF8, 0, arg, -1, out.data(), required, nullptr, nullptr);
        if (written <= 0) {
            throw std::runtime_error("Failed to convert command line argument to UTF-8.");
        }
        if (!out.empty() && out.back() == '\0') {
            out.pop_back();
        }
        return out;
    }
#else
    using NativeArg = char*;

    std::string nativeArgToUtf8(const char* arg) {
        return arg == nullptr ? std::string() : std::string(arg);
    }
#endif

    std::vector<std::string> collectGuiArguments(int argc, NativeArg argv[]) {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            args.push_back(nativeArgToUtf8(argv[i]));
        }
        if (args.empty()) {
            // argv[0] 占位符，QApplication 用它作窗口类名前缀。
            args.emplace_back("mcdk-gui");
        }
        return args;
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
        auto config = mcdk::userParseConfig();
#ifdef MCDK_ENABLE_CLI
        // 决策 8：GUI 版也保留 CLI 子模式（默认 OFF）。
        // 启用时若首参为 CLI 子命令则走 CLI 路径，由 cli.cpp (CLI11) 决定是否真走 CLI。
        if (argc > 1) {
            return MCDK_CLI_PARSE(argc, argv);
        }
#endif
        auto               argStorage = collectGuiArguments(argc, argv);
        std::vector<char*> guiArgv;
        guiArgv.reserve(argStorage.size());
        for (auto& arg : argStorage) {
            guiArgv.push_back(arg.data());
        }
        int guiArgc = static_cast<int>(guiArgv.size());
        return mcdk::gui::runGui(guiArgc, guiArgv.data(), config);
#ifdef NDEBUG
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }
#endif
    return 0;
}
