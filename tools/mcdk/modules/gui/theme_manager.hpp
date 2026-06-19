#pragma once

class QApplication;

namespace mcdk::gui {

    class ThemeManager {
    public:
        static void apply(QApplication& app);
    };

} // namespace mcdk::gui
