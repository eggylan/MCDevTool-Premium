#pragma once

#include <QMainWindow>

#include <nlohmann/json.hpp>

namespace mcdk {
    class CoreServices;
    class GameLauncher;
} // namespace mcdk

namespace mcdk::gui {

    class MainWindow : public QMainWindow {
    public:
        explicit MainWindow(
            const nlohmann::json& config,
            CoreServices&         core,
            GameLauncher&         launcher,
            QWidget*              parent = nullptr
        );

    private:
        CoreServices& core_;
        GameLauncher& launcher_;
    };

} // namespace mcdk::gui
