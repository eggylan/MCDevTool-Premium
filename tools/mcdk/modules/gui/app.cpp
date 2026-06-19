#include "app.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QString>

#include "../core_services.hpp"
#include "../game_launcher.hpp"
#include "log_bridge.hpp"
#include "main_window.hpp"
#include "theme_manager.hpp"

namespace mcdk::gui {

    int runGui(int argc, char* argv[], const nlohmann::json& config) {
        QApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("mcdk"));
        QCoreApplication::setApplicationVersion(QStringLiteral("0.1"));
        QCoreApplication::setOrganizationName(QStringLiteral("MCDevTool"));

        ThemeManager::apply(app);

        LogBridge logBridge;
        CoreServices core(config, [&logBridge](const std::string& line, ConsoleColor color) {
            CoreServices::printColoredAtomic(line, color);
            logBridge.publish(line, color);
        });
        GameLauncher launcher;
        MainWindow   window(config, core, launcher, logBridge);
        window.show();

        return QApplication::exec();
    }

} // namespace mcdk::gui
