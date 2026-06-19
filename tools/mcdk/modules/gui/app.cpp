#include "app.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QString>

#include "../core_services.hpp"
#include "../game_launcher.hpp"
#include "main_window.hpp"
#include "theme_manager.hpp"

namespace mcdk::gui {

    int runGui(int argc, char* argv[], const nlohmann::json& config) {
        QApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("mcdk"));
        QCoreApplication::setApplicationVersion(QStringLiteral("0.1"));
        QCoreApplication::setOrganizationName(QStringLiteral("MCDevTool"));

        ThemeManager::apply(app);

        CoreServices core(config);
        GameLauncher launcher;
        MainWindow   window(config, core, launcher);
        window.show();

        return QApplication::exec();
    }

} // namespace mcdk::gui
