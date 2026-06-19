#include "log_console_panel.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QIODevice>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QKeySequence>
#include <QStringList>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextBlockUserData>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QtGlobal>
#include <QVBoxLayout>

#include "theme_manager.hpp"

namespace mcdk::gui {
namespace {

    class LogBlockData : public QTextBlockUserData {
    public:
        explicit LogBlockData(LogLevel value)
        : level(value) {}

        LogLevel level;
    };

    class LogHighlighter : public QSyntaxHighlighter {
    public:
        explicit LogHighlighter(QTextDocument* document)
        : QSyntaxHighlighter(document) {}

    protected:
        void highlightBlock(const QString& text) override {
            LogLevel level = classifyLogLine(text);
            if (auto* data = dynamic_cast<LogBlockData*>(currentBlock().userData())) {
                level = data->level;
            }

            QTextCharFormat format;
            format.setForeground(ThemeManager::logColor(level));
            setFormat(0, text.length(), format);
        }
    };

    QString normalizeLogLine(QString line) {
        line.replace(QLatin1Char('\r'), QLatin1Char(' '));
        line.replace(QLatin1Char('\n'), QLatin1Char(' '));
        return line;
    }

    int levelToComboValue(LogLevel level) {
        return static_cast<int>(level);
    }

    LogLevel comboValueToLevel(int value) {
        if (value < static_cast<int>(LogLevel::Normal) || value > static_cast<int>(LogLevel::Developer)) {
            return LogLevel::Normal;
        }
        return static_cast<LogLevel>(value);
    }

    void setLastBlockLevel(QPlainTextEdit* viewer, LogLevel level, QSyntaxHighlighter* highlighter) {
        QTextBlock block = viewer->document()->lastBlock();
        if (!block.isValid()) {
            return;
        }
        block.setUserData(new LogBlockData(level));
        if (highlighter) {
            highlighter->rehighlightBlock(block);
        }
    }

    void setBlockLevels(QPlainTextEdit* viewer, const std::vector<LogEntry>& entries, QSyntaxHighlighter* highlighter) {
        QTextBlock  block = viewer->document()->firstBlock();
        std::size_t index = 0;
        while (block.isValid() && index < entries.size()) {
            block.setUserData(new LogBlockData(entries[index].level));
            block = block.next();
            ++index;
        }
        if (highlighter) {
            highlighter->rehighlight();
        }
    }

} // namespace

    LogConsolePanel::LogConsolePanel(QWidget* parent)
    : QWidget(parent) {
        setupUi();
    }

    void LogConsolePanel::appendLine(QString line, int level) {
        LogLevel logLevel = comboValueToLevel(level);
        LogEntry entry{normalizeLogLine(std::move(line)), logLevel};
        if (entry.level == LogLevel::Normal) {
            entry.level = classifyLogLine(entry.line, entry.level);
        }

        entries_.push_back(entry);
        bool trimmed = false;
        if (entries_.size() > static_cast<std::size_t>(kMaxEntries)) {
            entries_.erase(
                entries_.begin(),
                entries_.begin() + static_cast<std::ptrdiff_t>(entries_.size() - static_cast<std::size_t>(kMaxEntries))
            );
            trimmed = true;
        }

        if (trimmed) {
            rebuildView();
            return;
        }

        if (entryMatchesFilters(entry)) {
            appendEntryToView(entry);
            updateSearchState();
        } else {
            updateStatus();
        }
    }

    void LogConsolePanel::appendEntries(const std::vector<LogEntry>& entries) {
        for (const auto& entry : entries) {
            LogEntry copy = entry;
            copy.line = normalizeLogLine(std::move(copy.line));
            if (copy.level == LogLevel::Normal) {
                copy.level = classifyLogLine(copy.line, copy.level);
            }
            entries_.push_back(std::move(copy));
        }
        if (entries_.size() > static_cast<std::size_t>(kMaxEntries)) {
            entries_.erase(
                entries_.begin(),
                entries_.begin() + static_cast<std::ptrdiff_t>(entries_.size() - static_cast<std::size_t>(kMaxEntries))
            );
        }
        rebuildView();
    }

    void LogConsolePanel::setupUi() {
        setObjectName(QStringLiteral("LogConsolePanel"));

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(8);

        auto* toolbar = new QWidget(this);
        toolbar->setObjectName(QStringLiteral("LogConsoleToolbar"));
        auto* toolbarLayout = new QVBoxLayout(toolbar);
        toolbarLayout->setContentsMargins(10, 8, 10, 8);
        toolbarLayout->setSpacing(6);

        auto* searchRow = new QHBoxLayout();
        searchRow->setContentsMargins(0, 0, 0, 0);
        searchRow->setSpacing(8);

        auto* filterRow = new QHBoxLayout();
        filterRow->setContentsMargins(0, 0, 0, 0);
        filterRow->setSpacing(8);

        searchEdit_ = new QLineEdit(toolbar);
        searchEdit_->setPlaceholderText(QStringLiteral("搜索日志"));
        searchEdit_->setClearButtonEnabled(true);

        matchLabel_ = new QLabel(QStringLiteral("匹配 0"), toolbar);
        matchLabel_->setObjectName(QStringLiteral("LogMatchLabel"));

        previousButton_ = new QPushButton(QStringLiteral("上一个"), toolbar);
        nextButton_ = new QPushButton(QStringLiteral("下一个"), toolbar);
        previousButton_->setEnabled(false);
        nextButton_->setEnabled(false);

        caseCheck_ = new QCheckBox(QStringLiteral("区分大小写"), toolbar);
        pythonOnlyCheck_ = new QCheckBox(QStringLiteral("只看 Python"), toolbar);
        autoScrollCheck_ = new QCheckBox(QStringLiteral("自动滚动"), toolbar);
        autoScrollCheck_->setChecked(true);

        levelCombo_ = new QComboBox(toolbar);
        levelCombo_->addItem(QStringLiteral("全部级别"), -1);
        levelCombo_->addItem(QStringLiteral("错误"), levelToComboValue(LogLevel::Error));
        levelCombo_->addItem(QStringLiteral("警告"), levelToComboValue(LogLevel::Warning));
        levelCombo_->addItem(QStringLiteral("调试"), levelToComboValue(LogLevel::Debug));
        levelCombo_->addItem(QStringLiteral("成功"), levelToComboValue(LogLevel::Success));
        levelCombo_->addItem(QStringLiteral("Developer"), levelToComboValue(LogLevel::Developer));
        levelCombo_->addItem(QStringLiteral("普通"), levelToComboValue(LogLevel::Normal));

        fontSizeCombo_ = new QComboBox(toolbar);
        for (int size : {10, 11, 12, 13, 14, 16, 18}) {
            fontSizeCombo_->addItem(QString::number(size), size);
        }
        fontSizeCombo_->setCurrentIndex(2);

        auto* clearButton = new QPushButton(QStringLiteral("清空"), toolbar);
        auto* copyButton = new QPushButton(QStringLiteral("复制"), toolbar);
        auto* exportButton = new QPushButton(QStringLiteral("导出"), toolbar);

        searchRow->addWidget(searchEdit_, 1);
        searchRow->addWidget(matchLabel_);
        searchRow->addWidget(previousButton_);
        searchRow->addWidget(nextButton_);
        searchRow->addWidget(caseCheck_);

        filterRow->addWidget(pythonOnlyCheck_);
        filterRow->addWidget(levelCombo_);
        filterRow->addWidget(autoScrollCheck_);
        filterRow->addWidget(new QLabel(QStringLiteral("字号"), toolbar));
        filterRow->addWidget(fontSizeCombo_);
        filterRow->addStretch(1);
        filterRow->addWidget(clearButton);
        filterRow->addWidget(copyButton);
        filterRow->addWidget(exportButton);

        toolbarLayout->addLayout(searchRow);
        toolbarLayout->addLayout(filterRow);

        viewer_ = new QPlainTextEdit(this);
        viewer_->setObjectName(QStringLiteral("LogViewer"));
        viewer_->setReadOnly(true);
        viewer_->setLineWrapMode(QPlainTextEdit::NoWrap);
        viewer_->setMaximumBlockCount(kMaxEntries);
        viewer_->setCenterOnScroll(false);
        viewer_->setPlaceholderText(QStringLiteral("等待日志..."));
        viewer_->setUndoRedoEnabled(false);
        QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        font.setPointSize(fontSizeCombo_->currentData().toInt());
        viewer_->setFont(font);
        highlighter_ = new LogHighlighter(viewer_->document());

        statusLabel_ = new QLabel(QStringLiteral("显示 0 / 0"), this);
        statusLabel_->setObjectName(QStringLiteral("LogStatusLabel"));

        root->addWidget(toolbar);
        root->addWidget(viewer_, 1);
        root->addWidget(statusLabel_);

        connect(searchEdit_, &QLineEdit::textChanged, this, [this]() { updateSearchState(); });
        connect(caseCheck_, &QCheckBox::toggled, this, [this]() { updateSearchState(); });
        connect(pythonOnlyCheck_, &QCheckBox::toggled, this, [this]() { rebuildView(); });
        connect(levelCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { rebuildView(); });
        connect(fontSizeCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { applyFontSize(); });
        connect(previousButton_, &QPushButton::clicked, this, [this]() { findMatch(true); });
        connect(nextButton_, &QPushButton::clicked, this, [this]() { findMatch(false); });
        connect(clearButton, &QPushButton::clicked, this, [this]() { clearEntries(); });
        connect(copyButton, &QPushButton::clicked, this, [this]() { copySelectionOrVisibleText(); });
        connect(exportButton, &QPushButton::clicked, this, [this]() { exportVisibleText(); });
        connect(viewer_->verticalScrollBar(), &QScrollBar::sliderPressed, this, [this]() {
            autoScrollCheck_->setChecked(false);
        });

        auto* findShortcut = new QShortcut(QKeySequence::Find, this);
        findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(findShortcut, &QShortcut::activated, this, [this]() {
            searchEdit_->setFocus(Qt::ShortcutFocusReason);
            searchEdit_->selectAll();
        });

        auto* nextShortcut = new QShortcut(QKeySequence(QStringLiteral("F3")), this);
        nextShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(nextShortcut, &QShortcut::activated, this, [this]() { findMatch(false); });

        auto* previousShortcut = new QShortcut(QKeySequence(QStringLiteral("Shift+F3")), this);
        previousShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(previousShortcut, &QShortcut::activated, this, [this]() { findMatch(true); });
    }

    void LogConsolePanel::appendEntryToView(const LogEntry& entry) {
        viewer_->appendPlainText(entry.line);
        setLastBlockLevel(viewer_, entry.level, highlighter_);
        if (autoScrollCheck_->isChecked()) {
            viewer_->verticalScrollBar()->setValue(viewer_->verticalScrollBar()->maximum());
        }
    }

    bool LogConsolePanel::entryMatchesFilters(const LogEntry& entry) const {
        if (pythonOnlyCheck_->isChecked() && !entry.line.contains(QStringLiteral("[Python] "))) {
            return false;
        }

        int selectedLevel = levelCombo_->currentData().toInt();
        if (selectedLevel >= 0 && selectedLevel != static_cast<int>(entry.level)) {
            return false;
        }

        return true;
    }

    void LogConsolePanel::rebuildView() {
        QStringList lines;
        std::vector<LogEntry> visibleEntries;
        visibleEntries.reserve(entries_.size());
        lines.reserve(static_cast<int>(entries_.size()));
        for (const auto& entry : entries_) {
            if (entryMatchesFilters(entry)) {
                visibleEntries.push_back(entry);
                lines.push_back(entry.line);
            }
        }
        viewer_->setPlainText(lines.join(QLatin1Char('\n')));
        setBlockLevels(viewer_, visibleEntries, highlighter_);
        if (autoScrollCheck_->isChecked()) {
            viewer_->verticalScrollBar()->setValue(viewer_->verticalScrollBar()->maximum());
        }
        updateSearchState();
    }

    void LogConsolePanel::updateSearchState() {
        int matches = searchMatchCount();
        matchLabel_->setText(QStringLiteral("匹配 %1").arg(matches));
        bool canNavigate = matches > 0;
        previousButton_->setEnabled(canNavigate);
        nextButton_->setEnabled(canNavigate);
        updateStatus();
    }

    void LogConsolePanel::updateStatus() {
        statusLabel_->setText(
            QStringLiteral("显示 %1 / %2").arg(visibleEntryCount()).arg(static_cast<int>(entries_.size()))
        );
    }

    void LogConsolePanel::findMatch(bool backwards) {
        QString needle = searchEdit_->text();
        if (needle.isEmpty()) {
            return;
        }

        QTextDocument::FindFlags flags;
        if (caseCheck_->isChecked()) {
            flags |= QTextDocument::FindCaseSensitively;
        }
        if (backwards) {
            flags |= QTextDocument::FindBackward;
        }

        if (viewer_->find(needle, flags)) {
            return;
        }

        QTextCursor cursor = viewer_->textCursor();
        cursor.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
        viewer_->setTextCursor(cursor);
        viewer_->find(needle, flags);
    }

    void LogConsolePanel::clearEntries() {
        entries_.clear();
        viewer_->clear();
        updateSearchState();
    }

    void LogConsolePanel::copySelectionOrVisibleText() {
        if (viewer_->textCursor().hasSelection()) {
            viewer_->copy();
            return;
        }
        QApplication::clipboard()->setText(viewer_->toPlainText());
    }

    void LogConsolePanel::exportVisibleText() {
        QString defaultName = QStringLiteral("mcdk-log-%1.txt").arg(
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"))
        );
        QString fileName = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("导出日志"),
            QDir::current().filePath(defaultName),
            QStringLiteral("Text files (*.txt);;All files (*)")
        );
        if (fileName.isEmpty()) {
            return;
        }

        QFile file(fileName);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
            return;
        }
        const QByteArray data = viewer_->toPlainText().toUtf8();
        if (file.write(data) != data.size()) {
            QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
            return;
        }
    }

    void LogConsolePanel::applyFontSize() {
        QFont font = viewer_->font();
        font.setPointSize(fontSizeCombo_->currentData().toInt());
        viewer_->setFont(font);
    }

    int LogConsolePanel::visibleEntryCount() const {
        return static_cast<int>(std::count_if(entries_.begin(), entries_.end(), [this](const LogEntry& entry) {
            return entryMatchesFilters(entry);
        }));
    }

    int LogConsolePanel::searchMatchCount() const {
        QString needle = searchEdit_->text();
        if (needle.isEmpty()) {
            return 0;
        }

        QString text = viewer_->toPlainText();
        Qt::CaseSensitivity sensitivity = caseCheck_->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive;
        int count = 0;
        qsizetype pos = 0;
        while ((pos = text.indexOf(needle, pos, sensitivity)) >= 0) {
            ++count;
            pos += std::max<qsizetype>(1, needle.length());
        }
        return count;
    }

} // namespace mcdk::gui
