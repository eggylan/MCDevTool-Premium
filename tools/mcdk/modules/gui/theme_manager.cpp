#include "theme_manager.hpp"

#include <iterator>

#include <QApplication>
#include <QFont>
#include <QString>

namespace mcdk::gui {

    void ThemeManager::apply(QApplication& app) {
        QFont font = app.font();
        font.setPointSize(10);
        app.setFont(font);

        app.setStyleSheet(QStringLiteral(R"(
            QMainWindow {
                background: #f7f8fa;
            }

            QWidget#MainWindowCentral {
                background: #f7f8fa;
            }

            QLabel#MainWindowTitle {
                color: #172033;
                font-size: 28px;
                font-weight: 600;
            }

            QLabel#MainWindowState {
                color: #667085;
                font-size: 13px;
            }

            QStatusBar {
                color: #475467;
                background: #ffffff;
                border-top: 1px solid #d9dee8;
            }

            QWidget#LogConsolePanel {
                background: #ffffff;
            }

            QWidget#LogConsoleToolbar {
                background: #ffffff;
                border: 1px solid #d9dee8;
                border-radius: 6px;
            }

            QPlainTextEdit#LogViewer {
                color: #1d2939;
                background: #101828;
                border: 1px solid #202939;
                border-radius: 6px;
                selection-background-color: #2e90fa;
                selection-color: #ffffff;
            }

            QLabel#LogStatusLabel,
            QLabel#LogMatchLabel {
                color: #667085;
            }

            QWidget#CodeExecPanel {
                background: #ffffff;
            }

            QPlainTextEdit#CodeEditor {
                color: #e4e7ec;
                background: #101828;
                border: 1px solid #202939;
                border-radius: 6px;
                selection-background-color: #2e90fa;
                selection-color: #ffffff;
            }

            QLabel#CodeExecStatusLabel {
                color: #667085;
            }
        )"));
    }

    QColor ThemeManager::logColor(LogLevel level) {
        static const QColor kColors[] = {
            QColor(QStringLiteral("#e4e7ec")), // Normal
            QColor(QStringLiteral("#ff6b6b")), // Error
            QColor(QStringLiteral("#ffd166")), // Warning
            QColor(QStringLiteral("#5cc8ff")), // Debug
            QColor(QStringLiteral("#55d187")), // Success
            QColor(QStringLiteral("#a8b3cf")), // Developer
        };
        const auto idx = static_cast<int>(level);
        if (idx < 0 || idx >= static_cast<int>(std::size(kColors))) {
            return kColors[0];
        }
        return kColors[idx];
    }

    QColor ThemeManager::syntaxColor(SyntaxRole role) {
        static const QColor kColors[] = {
            QColor(QStringLiteral("#c792ea")), // Keyword
            QColor(QStringLiteral("#82aaff")), // Builtin
            QColor(QStringLiteral("#c3e88d")), // String
            QColor(QStringLiteral("#5c6788")), // Comment
            QColor(QStringLiteral("#f78c6c")), // Number
            QColor(QStringLiteral("#ffcb6b")), // Decorator
            QColor(QStringLiteral("#82aaff")), // Definition
        };
        const auto idx = static_cast<int>(role);
        if (idx < 0 || idx >= static_cast<int>(std::size(kColors))) {
            return kColors[0];
        }
        return kColors[idx];
    }

    QColor ThemeManager::editorColor(EditorRole role) {
        static const QColor kColors[] = {
            QColor(QStringLiteral("#101828")), // Background (mirrors QPlainTextEdit#CodeEditor QSS)
            QColor(QStringLiteral("#e4e7ec")), // Foreground
            QColor(QStringLiteral("#0b1220")), // GutterBackground
            QColor(QStringLiteral("#5b667d")), // GutterForeground
            QColor(QStringLiteral("#18233b")), // CurrentLine
        };
        const auto idx = static_cast<int>(role);
        if (idx < 0 || idx >= static_cast<int>(std::size(kColors))) {
            return kColors[0];
        }
        return kColors[idx];
    }

} // namespace mcdk::gui
