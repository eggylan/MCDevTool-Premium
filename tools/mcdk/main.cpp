// MCDK
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
#include "modules/core_services.hpp"
#include "modules/env.hpp"
#include "modules/game_launcher.hpp"

#ifdef MCDK_ENABLE_GUI
#include "modules/gui/app.hpp"
#endif

#ifdef MCDK_ENABLE_CLI
#ifdef _WIN32
int MCDK_CLI_PARSE(int argc, wchar_t* argv[]);
#else
int MCDK_CLI_PARSE(int argc, char* argv[]);
#endif
#endif

namespace {

    enum class StartupMode {
        Default,
        Gui,
        NoGui,
    };

#ifdef _WIN32
    using NativeArg = wchar_t*;

    bool argEquals(const wchar_t* arg, const wchar_t* expected) {
        return arg != nullptr && std::wcscmp(arg, expected) == 0;
    }

    bool isModeArgument(const wchar_t* arg) {
        return argEquals(arg, L"--gui") || argEquals(arg, L"--no-gui");
    }

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

    bool argEquals(const char* arg, const char* expected) {
        return arg != nullptr && std::strcmp(arg, expected) == 0;
    }

    bool isModeArgument(const char* arg) {
        return argEquals(arg, "--gui") || argEquals(arg, "--no-gui");
    }

    std::string nativeArgToUtf8(const char* arg) {
        return arg == nullptr ? std::string() : std::string(arg);
    }
#endif

    StartupMode parseStartupMode(int argc, NativeArg argv[]) {
        StartupMode mode = StartupMode::Default;
        for (int i = 1; i < argc; ++i) {
#ifdef _WIN32
            if (argEquals(argv[i], L"--gui")) {
#else
            if (argEquals(argv[i], "--gui")) {
#endif
                if (mode == StartupMode::NoGui) {
                    throw std::runtime_error("Cannot specify both --gui and --no-gui.");
                }
                mode = StartupMode::Gui;
                continue;
            }

#ifdef _WIN32
            if (argEquals(argv[i], L"--no-gui")) {
#else
            if (argEquals(argv[i], "--no-gui")) {
#endif
                if (mode == StartupMode::Gui) {
                    throw std::runtime_error("Cannot specify both --gui and --no-gui.");
                }
                mode = StartupMode::NoGui;
            }
        }
        return mode;
    }

    bool hasNonModeArguments(int argc, NativeArg argv[]) {
        for (int i = 1; i < argc; ++i) {
            if (!isModeArgument(argv[i])) {
                return true;
            }
        }
        return false;
    }

    bool configGuiValue(const nlohmann::json& config, bool defaultValue) {
        auto it = config.find("gui");
        if (it != config.end() && it->is_boolean()) {
            return it->get<bool>();
        }
        return defaultValue;
    }

    int runNoGui(const nlohmann::json& config) {
        mcdk::CoreServices core(config);
        mcdk::GameLauncher launcher;
        auto               prepared = launcher.prepareWorld(config);
        launcher.runBlocking(config, prepared, core);
        return 0;
    }

#ifdef MCDK_ENABLE_GUI
    bool shouldUseGui(const nlohmann::json& config, StartupMode mode) {
        if (mode == StartupMode::Gui) {
            return true;
        }
        if (mode == StartupMode::NoGui) {
            return false;
        }
        return configGuiValue(config, !mcdk::getEnvIsSubprocessMode());
    }

    std::vector<std::string> collectGuiArguments(int argc, NativeArg argv[]) {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            if (!isModeArgument(argv[i])) {
                args.push_back(nativeArgToUtf8(argv[i]));
            }
        }
        if (args.empty()) {
            args.emplace_back("mcdk");
        }
        return args;
    }

    int runGui(int argc, NativeArg argv[], const nlohmann::json& config) {
        auto               argStorage = collectGuiArguments(argc, argv);
        std::vector<char*> guiArgv;
        guiArgv.reserve(argStorage.size());
        for (auto& arg : argStorage) {
            guiArgv.push_back(arg.data());
        }
        int guiArgc = static_cast<int>(guiArgv.size());
        return mcdk::gui::runGui(guiArgc, guiArgv.data(), config);
    }
#endif

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
        StartupMode mode = parseStartupMode(argc, argv);
#ifdef MCDK_ENABLE_CLI
        if (hasNonModeArguments(argc, argv)) {
            return MCDK_CLI_PARSE(argc, argv);
        }
#endif

        auto config = mcdk::userParseConfig();
#ifdef MCDK_ENABLE_GUI
        if (shouldUseGui(config, mode)) {
            return runGui(argc, argv, config);
        }
#else
        if (mode == StartupMode::Gui) {
            throw std::runtime_error("--gui was requested, but this mcdk build has GUI support disabled.");
        }
#endif

        return runNoGui(config);
#ifdef NDEBUG
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }
#endif
    return 0;
}
