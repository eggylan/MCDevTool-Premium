#pragma once

#include <QColor>

#include "log_level.hpp"

class QApplication;

namespace mcdk::gui {

    class ThemeManager {
    public:
        static void apply(QApplication& app);
        static QColor logColor(LogLevel level);
    };

} // namespace mcdk::gui
