#include "theme_manager.hpp"

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
        )"));
    }

    QColor ThemeManager::logColor(LogLevel level) {
        switch (level) {
        case LogLevel::Error:
            return QColor(QStringLiteral("#ff6b6b"));
        case LogLevel::Warning:
            return QColor(QStringLiteral("#ffd166"));
        case LogLevel::Debug:
            return QColor(QStringLiteral("#5cc8ff"));
        case LogLevel::Success:
            return QColor(QStringLiteral("#55d187"));
        case LogLevel::Developer:
            return QColor(QStringLiteral("#a8b3cf"));
        case LogLevel::Normal:
        default:
            return QColor(QStringLiteral("#e4e7ec"));
        }
    }

} // namespace mcdk::gui
