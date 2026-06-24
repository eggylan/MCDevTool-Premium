#pragma once

#include <QMainWindow>

#include <nlohmann/json.hpp>

namespace mcdk {
    class CoreServices;
    class GameLauncher;
} // namespace mcdk

namespace mcdk::gui {
    class LogBridge;
    class LogConsolePanel;
    class CodeExecPanel;

    class MainWindow : public QMainWindow {
        Q_OBJECT

    public:
        explicit MainWindow(
            const nlohmann::json& config,
            CoreServices&         core,
            GameLauncher&         launcher,
            LogBridge&            logBridge,
            QWidget*              parent = nullptr
        );

    private:
        CoreServices& core_;
        GameLauncher& launcher_;
        CodeExecPanel*   codeExec_   = nullptr;
        LogConsolePanel* logConsole_ = nullptr;
    };

} // namespace mcdk::gui
