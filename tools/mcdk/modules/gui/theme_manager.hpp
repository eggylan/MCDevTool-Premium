#pragma once

#include <QColor>

#include "log_level.hpp"

class QApplication;

namespace mcdk::gui {

    // Syntax highlighting roles for the code editor. Widgets must resolve colors
    // through ThemeManager instead of hardcoding them (see design doc §8).
    enum class SyntaxRole {
        Keyword,
        Builtin,
        String,
        Comment,
        Number,
        Decorator,
        Definition,
    };

    // Structural colors for the code editor surface (background, gutter, etc.).
    enum class EditorRole {
        Background,
        Foreground,
        GutterBackground,
        GutterForeground,
        CurrentLine,
    };

    class ThemeManager {
    public:
        static void apply(QApplication& app);
        static QColor logColor(LogLevel level);
        static QColor syntaxColor(SyntaxRole role);
        static QColor editorColor(EditorRole role);
    };

} // namespace mcdk::gui
