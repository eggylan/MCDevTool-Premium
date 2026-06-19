#include "main_window.hpp"

#include <QLabel>
#include <QStatusBar>
#include <QString>
#include <vector>
#include <QVBoxLayout>
#include <QWidget>

#include "../core_services.hpp"
#include "../game_launcher.hpp"
#include "log_bridge.hpp"
#include "log_console_panel.hpp"

namespace mcdk::gui {

    MainWindow::MainWindow(
        const nlohmann::json& config,
        CoreServices&         core,
        GameLauncher&         launcher,
        LogBridge&            logBridge,
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
        layout->setSpacing(12);

        auto* title = new QLabel(QStringLiteral("日志控制台"), central);
        title->setObjectName(QStringLiteral("MainWindowTitle"));
        title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        layout->addWidget(title);

        auto* state = new QLabel(QStringLiteral("等待日志"), central);
        state->setObjectName(QStringLiteral("MainWindowState"));
        state->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        layout->addWidget(state);

        logConsole_ = new LogConsolePanel(central);
        layout->addWidget(logConsole_, 1);

        std::vector<LogEntry> history;
        if (core.logBuffer) {
            for (const auto& line : core.logBuffer->getLatest(1000)) {
                QString text = QString::fromStdString(line);
                history.push_back(LogEntry{text, classifyLogLine(text)});
            }
        }
        if (!history.empty()) {
            logConsole_->appendEntries(history);
        }
        logConsole_->appendEntries(logBridge.snapshot());
        connect(&logBridge, &LogBridge::lineArrived, logConsole_, &LogConsolePanel::appendLine, Qt::QueuedConnection);

        setCentralWidget(central);
        statusBar()->showMessage(QStringLiteral("日志控制台就绪"));
    }

} // namespace mcdk::gui
