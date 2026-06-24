#include "python_highlighter.hpp"

#include <QColor>
#include <QFont>
#include <QRegularExpressionMatch>
#include <QRegularExpressionMatchIterator>
#include <QStringList>

#include "theme_manager.hpp"

namespace mcdk::gui {
namespace {

    QTextCharFormat makeFormat(const QColor& color, bool bold = false, bool italic = false) {
        QTextCharFormat format;
        format.setForeground(color);
        if (bold) {
            format.setFontWeight(QFont::Bold);
        }
        if (italic) {
            format.setFontItalic(true);
        }
        return format;
    }

} // namespace

    PythonHighlighter::PythonHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent) {
        const QTextCharFormat keywordFormat   = makeFormat(ThemeManager::syntaxColor(SyntaxRole::Keyword), true);
        const QTextCharFormat builtinFormat   = makeFormat(ThemeManager::syntaxColor(SyntaxRole::Builtin));
        const QTextCharFormat numberFormat    = makeFormat(ThemeManager::syntaxColor(SyntaxRole::Number));
        const QTextCharFormat decoratorFormat = makeFormat(ThemeManager::syntaxColor(SyntaxRole::Decorator), false, true);
        const QTextCharFormat definitionFormat = makeFormat(ThemeManager::syntaxColor(SyntaxRole::Definition), true);

        stringFormat_          = makeFormat(ThemeManager::syntaxColor(SyntaxRole::String));
        commentFormat_         = makeFormat(ThemeManager::syntaxColor(SyntaxRole::Comment), false, true);
        multiLineStringFormat_ = stringFormat_;

        static const QString keywords = QStringLiteral(
            "False|None|True|and|as|assert|async|await|break|class|continue|def|del|elif|else|except|"
            "finally|for|from|global|if|import|in|is|lambda|nonlocal|not|or|pass|raise|return|try|while|with|yield"
        );
        static const QString builtins = QStringLiteral(
            "abs|all|any|bool|bytes|callable|chr|cls|dict|dir|enumerate|eval|exec|filter|float|format|frozenset|"
            "getattr|hasattr|hash|hex|id|input|int|isinstance|issubclass|iter|len|list|map|max|min|next|object|"
            "open|ord|pow|print|range|repr|reversed|round|self|set|setattr|sorted|str|sum|super|tuple|type|zip"
        );

        wordRules_.push_back({QRegularExpression(QStringLiteral("\\b(?:%1)\\b").arg(keywords)), keywordFormat, 0});
        wordRules_.push_back({QRegularExpression(QStringLiteral("\\b(?:%1)\\b").arg(builtins)), builtinFormat, 0});
        // Numbers: integers, floats, hex.
        wordRules_.push_back(
            {QRegularExpression(QStringLiteral("\\b(?:0[xX][0-9a-fA-F]+|\\d+\\.?\\d*(?:[eE][+-]?\\d+)?)\\b")),
             numberFormat,
             0}
        );
        // Decorator at line start.
        wordRules_.push_back({QRegularExpression(QStringLiteral("^\\s*@[\\w.]+")), decoratorFormat, 0});
        // Name following def/class.
        wordRules_.push_back(
            {QRegularExpression(QStringLiteral("\\b(?:def|class)\\s+(\\w+)")), definitionFormat, 1}
        );
    }

    void PythonHighlighter::applyWordRules(const QString& text, const std::vector<char>& masked) {
        const int length = static_cast<int>(text.length());
        for (const auto& rule : wordRules_) {
            QRegularExpressionMatchIterator it = rule.pattern.globalMatch(text);
            while (it.hasNext()) {
                const QRegularExpressionMatch match = it.next();
                const int start = static_cast<int>(match.capturedStart(rule.captureGroup));
                const int len   = static_cast<int>(match.capturedLength(rule.captureGroup));
                if (start < 0 || len <= 0 || start >= length) {
                    continue;
                }
                // Skip matches that fall inside a string or comment region.
                if (start < static_cast<int>(masked.size()) && masked[static_cast<std::size_t>(start)]) {
                    continue;
                }
                setFormat(start, len, rule.format);
            }
        }
    }

    void PythonHighlighter::highlightBlock(const QString& text) {
        const int        length = static_cast<int>(text.length());
        std::vector<char> masked(static_cast<std::size_t>(length), 0);
        auto              maskRange = [&](int from, int count) {
            for (int k = from; k < from + count && k < length; ++k) {
                if (k >= 0) {
                    masked[static_cast<std::size_t>(k)] = 1;
                }
            }
        };

        setCurrentBlockState(StateNormal);
        int index = 0;

        // Continuation of a multi-line triple-quoted string from the previous block.
        const int prevState = previousBlockState();
        if (prevState == StateInTripleSq || prevState == StateInTripleDq) {
            const QString delimiter = (prevState == StateInTripleSq) ? QStringLiteral("'''") : QStringLiteral("\"\"\"");
            const int     closeAt   = static_cast<int>(text.indexOf(delimiter));
            if (closeAt == -1) {
                setFormat(0, length, multiLineStringFormat_);
                maskRange(0, length);
                setCurrentBlockState(prevState);
                applyWordRules(text, masked);
                return;
            }
            const int consumed = closeAt + 3;
            setFormat(0, consumed, multiLineStringFormat_);
            maskRange(0, consumed);
            index = consumed;
        }

        while (index < length) {
            const QChar ch = text.at(index);

            if (ch == QLatin1Char('#')) {
                setFormat(index, length - index, commentFormat_);
                maskRange(index, length - index);
                break;
            }

            if (ch == QLatin1Char('\'') || ch == QLatin1Char('"')) {
                const QString triple = QString(3, ch);
                if (text.mid(index, 3) == triple) {
                    const int closeAt = static_cast<int>(text.indexOf(triple, index + 3));
                    if (closeAt == -1) {
                        setFormat(index, length - index, multiLineStringFormat_);
                        maskRange(index, length - index);
                        setCurrentBlockState(ch == QLatin1Char('\'') ? StateInTripleSq : StateInTripleDq);
                        index = length;
                        break;
                    }
                    const int consumed = closeAt + 3 - index;
                    setFormat(index, consumed, multiLineStringFormat_);
                    maskRange(index, consumed);
                    index += consumed;
                    continue;
                }

                // Single-line string with backslash escape handling.
                const QChar quote = ch;
                int         scan  = index + 1;
                bool        closed = false;
                while (scan < length) {
                    if (text.at(scan) == QLatin1Char('\\')) {
                        scan += 2;
                        continue;
                    }
                    if (text.at(scan) == quote) {
                        ++scan;
                        closed = true;
                        break;
                    }
                    ++scan;
                }
                const int end = closed ? scan : length;
                setFormat(index, end - index, stringFormat_);
                maskRange(index, end - index);
                index = end;
                continue;
            }

            ++index;
        }

        applyWordRules(text, masked);
    }

} // namespace mcdk::gui
