#pragma once

#include <vector>

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

class QTextDocument;

namespace mcdk::gui {

    // Lightweight Python syntax highlighter for the code-execution editor.
    // Handles keywords, builtins, numbers, decorators, def/class names,
    // single-line strings, comments, and multi-line triple-quoted strings.
    // All colors are resolved through ThemeManager (design doc §8).
    class PythonHighlighter : public QSyntaxHighlighter {
        Q_OBJECT

    public:
        explicit PythonHighlighter(QTextDocument* parent = nullptr);

    protected:
        void highlightBlock(const QString& text) override;

    private:
        // Block states for multi-line triple-quoted strings.
        enum BlockState : int {
            StateNormal      = 0,
            StateInTripleSq  = 1, // inside '''
            StateInTripleDq  = 2, // inside """
        };

        struct WordRule {
            QRegularExpression pattern;
            QTextCharFormat    format;
            int                captureGroup = 0;
        };

        void applyWordRules(const QString& text, const std::vector<char>& masked);

        std::vector<WordRule> wordRules_;
        QTextCharFormat       stringFormat_;
        QTextCharFormat       commentFormat_;
        QTextCharFormat       multiLineStringFormat_;
    };

} // namespace mcdk::gui
