#include "main_window.hpp"

#include <QLabel>
#include <QSplitter>
#include <QStatusBar>
#include <QString>
#include <QStringList>
#include <vector>
#include <QVBoxLayout>
#include <QWidget>

#include "../core_services.hpp"
#include "../game_launcher.hpp"
#include "code_exec_panel.hpp"
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
        (void)launcher_;

        setWindowTitle(QStringLiteral("MCDK"));
        resize(1180, 760);
        setMinimumSize(960, 620);

        auto* central = new QWidget(this);
        central->setObjectName(QStringLiteral("MainWindowCentral"));

        auto* layout = new QVBoxLayout(central);
        layout->setContentsMargins(24, 24, 24, 24);
        layout->setSpacing(12);

        auto* title = new QLabel(QStringLiteral("日志与调试工具"), central);
        title->setObjectName(QStringLiteral("MainWindowTitle"));
        title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        layout->addWidget(title);

        auto* state = new QLabel(QStringLiteral("调试控制台"), central);
        state->setObjectName(QStringLiteral("MainWindowState"));
        state->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        layout->addWidget(state);

        auto* splitter = new QSplitter(Qt::Horizontal, central);
        codeExec_   = new CodeExecPanel(core);
        logConsole_ = new LogConsolePanel();
        splitter->addWidget(codeExec_);
        splitter->addWidget(logConsole_);
        splitter->setStretchFactor(0, 3);
        splitter->setStretchFactor(1, 4);
        layout->addWidget(splitter, 1);

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

        // Code-execution results are routed into the log console. The result
        // text may be multi-line, so split it into one entry per line because
        // LogConsolePanel collapses embedded newlines within a single entry.
        connect(codeExec_, &CodeExecPanel::resultProduced, this, [this](QString text, int level) {
            const QStringList     parts    = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            const LogLevel        logLevel = level >= static_cast<int>(LogLevel::Normal)
                                          && level <= static_cast<int>(LogLevel::Developer)
                                                ? static_cast<LogLevel>(level)
                                                : LogLevel::Normal;
            std::vector<LogEntry> entries;
            entries.reserve(static_cast<std::size_t>(parts.size()));
            for (const auto& part : parts) {
                entries.push_back(LogEntry{part, logLevel});
            }
            logConsole_->appendEntries(entries);
        });

        setCentralWidget(central);
        statusBar()->showMessage(QStringLiteral("就绪"));
    }

} // namespace mcdk::gui
