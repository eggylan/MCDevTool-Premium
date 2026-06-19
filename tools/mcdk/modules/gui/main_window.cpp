#include "main_window.hpp"

#include <QLabel>
#include <QStatusBar>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include "../core_services.hpp"
#include "../game_launcher.hpp"

namespace mcdk::gui {

    MainWindow::MainWindow(
        const nlohmann::json& config,
        CoreServices&         core,
        GameLauncher&         launcher,
        QWidget*              parent
    )
    : QMainWindow(parent),
      core_(core),
      launcher_(launcher) {
        (void)config;
        (void)core_;
        (void)launcher_;

        setWindowTitle(QStringLiteral("MCDK"));
        resize(1180, 760);
        setMinimumSize(960, 620);

        auto* central = new QWidget(this);
        central->setObjectName(QStringLiteral("MainWindowCentral"));

        auto* layout = new QVBoxLayout(central);
        layout->setContentsMargins(24, 24, 24, 24);

        auto* title = new QLabel(QStringLiteral("MCDK"), central);
        title->setObjectName(QStringLiteral("MainWindowTitle"));
        title->setAlignment(Qt::AlignCenter);
        layout->addStretch(1);
        layout->addWidget(title);

        auto* state = new QLabel(QStringLiteral("GUI shell ready"), central);
        state->setObjectName(QStringLiteral("MainWindowState"));
        state->setAlignment(Qt::AlignCenter);
        layout->addWidget(state);
        layout->addStretch(1);

        setCentralWidget(central);
        statusBar()->showMessage(QStringLiteral("Ready"));
    }

} // namespace mcdk::gui
