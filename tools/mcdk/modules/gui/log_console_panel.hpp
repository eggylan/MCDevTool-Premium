#pragma once

#include <vector>

#include <QWidget>

#include "log_level.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSyntaxHighlighter;

namespace mcdk::gui {

    class LogConsolePanel : public QWidget {
        Q_OBJECT

    public:
        explicit LogConsolePanel(QWidget* parent = nullptr);

        void appendLine(QString line, int level);
        void appendEntries(const std::vector<LogEntry>& entries);

    private:
        static constexpr int kMaxEntries = 5000;

        void setupUi();
        void appendEntryToView(const LogEntry& entry);
        bool entryMatchesFilters(const LogEntry& entry) const;
        void rebuildView();
        void updateSearchState();
        void updateStatus();
        void findMatch(bool backwards);
        void clearEntries();
        void copySelectionOrVisibleText();
        void exportVisibleText();
        void applyFontSize();
        int  visibleEntryCount() const;
        int  searchMatchCount() const;

        std::vector<LogEntry> entries_;

        QLineEdit*      searchEdit_     = nullptr;
        QLabel*         matchLabel_     = nullptr;
        QLabel*         statusLabel_    = nullptr;
        QPushButton*    previousButton_ = nullptr;
        QPushButton*    nextButton_     = nullptr;
        QCheckBox*      caseCheck_      = nullptr;
        QCheckBox*      pythonOnlyCheck_ = nullptr;
        QCheckBox*      autoScrollCheck_ = nullptr;
        QComboBox*      levelCombo_     = nullptr;
        QComboBox*      fontSizeCombo_  = nullptr;
        QPlainTextEdit* viewer_         = nullptr;
        QSyntaxHighlighter* highlighter_ = nullptr;
    };

} // namespace mcdk::gui
