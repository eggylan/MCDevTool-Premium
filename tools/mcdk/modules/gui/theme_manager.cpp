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
        )"));
    }

} // namespace mcdk::gui
