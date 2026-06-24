#include "code_exec_panel.hpp"

#include <algorithm>
#include <sstream>
#include <string>

#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QRect>
#include <QSize>
#include <QTextBlock>
#include <QTextEdit>
#include <QTextFormat>
#include <QTimer>
#include <QVBoxLayout>
#include <QtGlobal>

#include <nlohmann/json.hpp>

#include "../core_services.hpp"
#include "log_level.hpp"
#include "python_highlighter.hpp"
#include "theme_manager.hpp"

namespace mcdk::gui {
namespace {

    // Companion widget that delegates painting/sizing back to the editor.
    class LineNumberArea : public QWidget {
    public:
        explicit LineNumberArea(CodeEditor* editor)
        : QWidget(editor),
          editor_(editor) {}

        QSize sizeHint() const override {
            return QSize(editor_->lineNumberAreaWidth(), 0);
        }

    protected:
        void paintEvent(QPaintEvent* event) override {
            editor_->lineNumberAreaPaintEvent(event);
        }

    private:
        CodeEditor* editor_;
    };

} // namespace

    // ---- CodeEditor -------------------------------------------------------

    CodeEditor::CodeEditor(QWidget* parent)
    : QPlainTextEdit(parent) {
        setObjectName(QStringLiteral("CodeEditor"));
        lineNumberArea_ = new LineNumberArea(this);

        QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        font.setPointSize(12);
        setFont(font);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);

        connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateLineNumberAreaWidth);
        connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateLineNumberArea);
        connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::highlightCurrentLine);

        updateLineNumberAreaWidth(0);
        highlightCurrentLine();
    }

    int CodeEditor::lineNumberAreaWidth() const {
        int digits = 1;
        int max    = (std::max)(1, blockCount());
        while (max >= 10) {
            max /= 10;
            ++digits;
        }
        return 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
    }

    void CodeEditor::updateLineNumberAreaWidth(int /*newBlockCount*/) {
        setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
    }

    void CodeEditor::updateLineNumberArea(const QRect& rect, int dy) {
        if (dy != 0) {
            lineNumberArea_->scroll(0, dy);
        } else {
            lineNumberArea_->update(0, rect.y(), lineNumberArea_->width(), rect.height());
        }
        if (rect.contains(viewport()->rect())) {
            updateLineNumberAreaWidth(0);
        }
    }

    void CodeEditor::resizeEvent(QResizeEvent* event) {
        QPlainTextEdit::resizeEvent(event);
        const QRect cr = contentsRect();
        lineNumberArea_->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
    }

    void CodeEditor::highlightCurrentLine() {
        QList<QTextEdit::ExtraSelection> extraSelections;
        if (!isReadOnly()) {
            QTextEdit::ExtraSelection selection;
            selection.format.setBackground(ThemeManager::editorColor(EditorRole::CurrentLine));
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selection.cursor = textCursor();
            selection.cursor.clearSelection();
            extraSelections.append(selection);
        }
        setExtraSelections(extraSelections);
    }

    void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent* event) {
        QPainter painter(lineNumberArea_);
        painter.fillRect(event->rect(), ThemeManager::editorColor(EditorRole::GutterBackground));

        QTextBlock block       = firstVisibleBlock();
        int        blockNumber = block.blockNumber();
        int        top         = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
        int        bottom      = top + qRound(blockBoundingRect(block).height());
        const int  paintWidth  = lineNumberArea_->width() - 6;

        painter.setPen(ThemeManager::editorColor(EditorRole::GutterForeground));
        while (block.isValid() && top <= event->rect().bottom()) {
            if (block.isVisible() && bottom >= event->rect().top()) {
                const QString number = QString::number(blockNumber + 1);
                painter.drawText(0, top, paintWidth, fontMetrics().height(), Qt::AlignRight, number);
            }
            block  = block.next();
            top    = bottom;
            bottom = top + qRound(blockBoundingRect(block).height());
            ++blockNumber;
        }
    }

    // ---- CodeExecWorker ---------------------------------------------------

    CodeExecWorker::CodeExecWorker(CoreServices& core, QObject* parent)
    : QObject(parent),
      core_(core) {}

    void CodeExecWorker::execute(QString code, bool isClient) {
        auto emitResult = [this](const QString& text, bool isError) {
            Q_EMIT finished(text, static_cast<int>(isError ? LogLevel::Error : LogLevel::Success));
        };

        auto ipc = core_.ipc;
        if (!ipc || ipc->getClientCount() == 0) {
            emitResult(QStringLiteral("[执行] 代码执行失败：游戏未连接或目标不可用。"), true);
            return;
        }

        nlohmann::json params = {
            {"code",      code.toStdString()},
            {"is_client", isClient}
        };
        const auto result = ipc->requestJson("execute_code", params.dump(), 10000);
        if (!result.success) {
            emitResult(
                QStringLiteral("[执行] 代码执行失败：%1").arg(QString::fromStdString(result.errorMessage)),
                true
            );
            return;
        }

        const auto response = nlohmann::json::parse(result.responseJson, nullptr, false);
        if (response.is_discarded() || !response.is_object()) {
            emitResult(
                QStringLiteral("[执行] 返回了无效 JSON：%1").arg(QString::fromStdString(result.responseJson)),
                true
            );
            return;
        }

        if (!response.value("ok", false)) {
            std::string message = response.dump();
            if (response.contains("error")) {
                const auto& error = response["error"];
                if (error.is_object() && error.contains("message")) {
                    message = error.value("message", message);
                }
            }
            emitResult(QStringLiteral("[执行] 代码执行失败：%1").arg(QString::fromStdString(message)), true);
            return;
        }

        nlohmann::json payload = nlohmann::json::object();
        if (response.contains("result")) {
            payload = response["result"];
        }

        std::ostringstream text;
        text << "[执行] 在" << (isClient ? "客户端" : "服务端") << "执行成功。";
        if (payload.is_object()) {
            if (payload.contains("return_type")) {
                text << "\n返回类型: " << payload.value("return_type", "unknown");
            }
            if (payload.contains("return_repr")) {
                text << "\n返回 repr: " << payload.value("return_repr", "");
            }
            if (payload.contains("return_value")) {
                text << "\n返回值 JSON: " << payload["return_value"].dump(2);
            }
        } else {
            text << "\n返回值 JSON: " << payload.dump(2);
        }
        emitResult(QString::fromStdString(text.str()), false);
    }

    // ---- CodeExecPanel ----------------------------------------------------

    CodeExecPanel::CodeExecPanel(CoreServices& core, QWidget* parent)
    : QWidget(parent),
      core_(core) {
        setupUi();
        new PythonHighlighter(editor_->document());

        worker_ = new CodeExecWorker(core_);
        worker_->moveToThread(&workerThread_);
        connect(this, &CodeExecPanel::requestExecute, worker_, &CodeExecWorker::execute, Qt::QueuedConnection);
        connect(worker_, &CodeExecWorker::finished, this, &CodeExecPanel::onExecutionFinished, Qt::QueuedConnection);
        workerThread_.start();

        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this]() { refreshConnectionState(); });
        timer->start(600);
        refreshConnectionState();
    }

    CodeExecPanel::~CodeExecPanel() {
        workerThread_.quit();
        workerThread_.wait();
        delete worker_;
    }

    void CodeExecPanel::setupUi() {
        setObjectName(QStringLiteral("CodeExecPanel"));

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(8);

        editor_ = new CodeEditor(this);
        editor_->setPlaceholderText(
            QStringLiteral("# 在此输入 Python 代码，点击\"客户端运行\"或\"服务端运行\"执行")
        );
        root->addWidget(editor_, 1);

        auto* presetRow = new QHBoxLayout();
        savePresetButton_ = new QPushButton(QStringLiteral("保存快捷指令"), this);
        loadPresetButton_ = new QPushButton(QStringLiteral("读取快捷指令"), this);
        savePresetButton_->setEnabled(false);
        loadPresetButton_->setEnabled(false);
        savePresetButton_->setToolTip(QStringLiteral("v1 暂未实现"));
        loadPresetButton_->setToolTip(QStringLiteral("v1 暂未实现"));
        presetRow->addWidget(savePresetButton_);
        presetRow->addWidget(loadPresetButton_);
        presetRow->addStretch(1);
        root->addLayout(presetRow);

        auto* runRow = new QHBoxLayout();
        runClientButton_ = new QPushButton(QStringLiteral("客户端运行"), this);
        runServerButton_ = new QPushButton(QStringLiteral("服务端运行"), this);
        runRow->addWidget(runClientButton_);
        runRow->addWidget(runServerButton_);
        runRow->addStretch(1);
        root->addLayout(runRow);

        auto* toolRow = new QHBoxLayout();
        componentButton_   = new QPushButton(QStringLiteral("组件管理"), this);
        uiDebugButton_     = new QPushButton(QStringLiteral("UI 调试"), this);
        customToolsButton_ = new QPushButton(QStringLiteral("自定义工具管理"), this);
        toolRow->addWidget(componentButton_);
        toolRow->addWidget(uiDebugButton_);
        toolRow->addWidget(customToolsButton_);
        toolRow->addStretch(1);
        root->addLayout(toolRow);

        statusLabel_ = new QLabel(this);
        statusLabel_->setObjectName(QStringLiteral("CodeExecStatusLabel"));
        root->addWidget(statusLabel_);

        connect(runClientButton_, &QPushButton::clicked, this, [this]() { runCode(true); });
        connect(runServerButton_, &QPushButton::clicked, this, [this]() { runCode(false); });
        connect(componentButton_, &QPushButton::clicked, this, [this]() {
            QMessageBox::information(this, QStringLiteral("组件管理"), QStringLiteral("组件管理：尚未实现。"));
        });
        connect(uiDebugButton_, &QPushButton::clicked, this, [this]() {
            QMessageBox::information(
                this, QStringLiteral("UI 调试"), QStringLiteral("UI 调试窗口将在后续阶段（P3-3）实现。")
            );
        });
        connect(customToolsButton_, &QPushButton::clicked, this, [this]() {
            QMessageBox::information(
                this, QStringLiteral("自定义工具管理"), QStringLiteral("自定义工具管理将在后续阶段（P3-5）实现。")
            );
        });
    }

    void CodeExecPanel::runCode(bool isClient) {
        if (running_) {
            return;
        }
        if (!connected_) {
            statusLabel_->setText(QStringLiteral("未连接游戏，无法执行。"));
            return;
        }
        const QString code = editor_->toPlainText();
        if (code.trimmed().isEmpty()) {
            statusLabel_->setText(QStringLiteral("请输入要执行的代码。"));
            return;
        }
        setRunningState(true);
        Q_EMIT requestExecute(code, isClient);
    }

    void CodeExecPanel::onExecutionFinished(QString text, int level) {
        setRunningState(false);
        Q_EMIT resultProduced(text, level);
        refreshConnectionState();
    }

    void CodeExecPanel::refreshConnectionState() {
        connected_           = core_.ipc && core_.ipc->getClientCount() > 0;
        const bool canRun = connected_ && !running_;
        runClientButton_->setEnabled(canRun);
        runServerButton_->setEnabled(canRun);
        if (running_) {
            statusLabel_->setText(QStringLiteral("正在执行代码..."));
        } else if (connected_) {
            statusLabel_->setText(QStringLiteral("已连接游戏，可执行代码。"));
        } else {
            statusLabel_->setText(QStringLiteral("未连接游戏（启动游戏并进入存档后可执行）。"));
        }
    }

    void CodeExecPanel::setRunningState(bool running) {
        running_ = running;
        if (running_) {
            runClientButton_->setEnabled(false);
            runServerButton_->setEnabled(false);
            statusLabel_->setText(QStringLiteral("正在执行代码..."));
        } else {
            const bool canRun = connected_ && !running_;
            runClientButton_->setEnabled(canRun);
            runServerButton_->setEnabled(canRun);
        }
    }

} // namespace mcdk::gui
