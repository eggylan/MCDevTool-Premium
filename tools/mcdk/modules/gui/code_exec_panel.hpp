#pragma once

#include <QPlainTextEdit>
#include <QThread>
#include <QWidget>

class QLabel;
class QPaintEvent;
class QPushButton;
class QResizeEvent;
class QRect;

namespace mcdk {
    class CoreServices;
} // namespace mcdk

namespace mcdk::gui {

    // QPlainTextEdit subclass with a line-number gutter and current-line
    // highlight, following the canonical Qt "Code Editor" pattern.
    class CodeEditor : public QPlainTextEdit {
        Q_OBJECT

    public:
        explicit CodeEditor(QWidget* parent = nullptr);

        void lineNumberAreaPaintEvent(QPaintEvent* event);
        int  lineNumberAreaWidth() const;

    protected:
        void resizeEvent(QResizeEvent* event) override;

    private:
        void updateLineNumberAreaWidth(int newBlockCount);
        void updateLineNumberArea(const QRect& rect, int dy);
        void highlightCurrentLine();

        QWidget* lineNumberArea_ = nullptr;
    };

    // Runs execute_code on a dedicated thread so the 10s blocking IPC request
    // never stalls the GUI thread. Lives in CodeExecPanel's worker thread.
    class CodeExecWorker : public QObject {
        Q_OBJECT

    public:
        explicit CodeExecWorker(CoreServices& core, QObject* parent = nullptr);

    public Q_SLOTS:
        void execute(QString code, bool isClient);

    Q_SIGNALS:
        void finished(QString text, int level);

    private:
        CoreServices& core_;
    };

    // Left pane of the main window: Python code editor + run controls. Sends
    // code to the game via execute_code and reports results through a signal.
    class CodeExecPanel : public QWidget {
        Q_OBJECT

    public:
        explicit CodeExecPanel(CoreServices& core, QWidget* parent = nullptr);
        ~CodeExecPanel() override;

    Q_SIGNALS:
        // Marshalled to the worker thread.
        void requestExecute(QString code, bool isClient);
        // Emitted on the GUI thread; carries one (possibly multi-line) result
        // block plus a LogLevel int for the log console to render.
        void resultProduced(QString text, int level);

    private:
        void setupUi();
        void runCode(bool isClient);
        void onExecutionFinished(QString text, int level);
        void refreshConnectionState();
        void setRunningState(bool running);

        CoreServices& core_;

        CodeEditor*  editor_            = nullptr;
        QPushButton* savePresetButton_  = nullptr;
        QPushButton* loadPresetButton_  = nullptr;
        QPushButton* runClientButton_   = nullptr;
        QPushButton* runServerButton_   = nullptr;
        QPushButton* componentButton_   = nullptr;
        QPushButton* uiDebugButton_     = nullptr;
        QPushButton* customToolsButton_ = nullptr;
        QLabel*      statusLabel_       = nullptr;

        QThread         workerThread_;
        CodeExecWorker* worker_ = nullptr;

        bool running_   = false;
        bool connected_ = false;
    };

} // namespace mcdk::gui
