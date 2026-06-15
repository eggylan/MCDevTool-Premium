#pragma once
// 游戏日志处理器：消费 Safaia 协议 4(MCProtocol::message)的原始 payload，
// 完成流式分行、[Python]/Engine 过滤、颜色与错误分类、控制台输出与日志缓冲。
//
// 从旧的 main.cpp 匿名管道 processStdout/processStderr lambda 中提取，便于独立单测。
// 输入来源由调用方注入(原为 stdout/stderr 管道，现为 SafaiaController message handler)。
//
// 线程模型：consume() 由 Safaia 客户端读线程调用，flush() 由主线程在 controller 停止后调用；
// 内部以 mutex 串行化，残留缓冲与 traceback 状态机均受其保护。
#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>

#include "./console.hpp"
#include "./log_buffer.hpp"
#include "./utils.hpp"

namespace mcdk {

    class GameLogProcessor {
    public:
        // 控制台输出回调：实际着色由调用方实现(测试时可捕获)。
        using ConsoleSink = std::function<void(const std::string& line, ConsoleColor color)>;

        // filterPython     : include_debug_mod，仅显示含 "[Python] " 的行。
        // logBuffer        : 普通日志缓冲(所有显示行)。可空。
        // errBuffer        : 错误日志缓冲(错误行与 traceback 块)。可空。
        // consoleSink      : 控制台输出回调。可空(纯缓冲场景)。
        // enableBuffering  : 是否写入 log/err 缓冲(MCP 关闭时仍输出控制台但不缓冲)。
        GameLogProcessor(
            bool                       filterPython,
            std::shared_ptr<LogBuffer> logBuffer,
            std::shared_ptr<LogBuffer> errBuffer,
            ConsoleSink                consoleSink,
            bool                       enableBuffering
        )
        : filterPython_(filterPython),
          enableBuffering_(enableBuffering),
          logBuffer_(std::move(logBuffer)),
          errBuffer_(std::move(errBuffer)),
          consoleSink_(std::move(consoleSink)) {}

        GameLogProcessor(const GameLogProcessor&)            = delete;
        GameLogProcessor& operator=(const GameLogProcessor&) = delete;

        // 喂入一段原始 payload，可能包含半行、多行或 CRLF。完整行立即处理，残留留待下次/flush。
        void consume(std::string_view chunk) {
            std::lock_guard<std::mutex> lk(mutex_);
            residual_.append(chunk.data(), chunk.size());
            size_t pos;
            while ((pos = residual_.find('\n')) != std::string::npos) {
                std::string line = residual_.substr(0, pos);
                residual_.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                processLine(line);
            }
        }

        // 断连或退出时处理最后一个无换行残行。
        void flush() {
            std::lock_guard<std::mutex> lk(mutex_);
            if (residual_.empty()) {
                return;
            }
            std::string line = std::move(residual_);
            residual_.clear();
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            processLine(line);
        }

    private:
        // ── 单行处理(在 mutex_ 持有下调用)──
        void processLine(const std::string& line) {
            // 1. include_debug_mod=true 且不含 "[Python] "：丢弃。
            if (filterPython_ && line.find("[Python] ") == std::string::npos) {
                return;
            }
            // 2. Engine INFO 噪音：丢弃。
            if (line.find(" [INFO][Engine] ") != std::string::npos) {
                return;
            }

            // 用于终止/空行/起始锚定的去前缀视图(去掉一个前导 "[Python] ")。
            std::string_view classify = stripPythonPrefix(line);

            bool startsTraceback = line.find("Traceback (most recent call last):") != std::string::npos;
            bool inTbThisLine    = inTraceback_ || startsTraceback;

            // 3. 路径转换：traceback 行执行旧的 File "a.b.c" -> "a/b/c.py" 转换。
            std::string display = inTbThisLine ? convertDottedFilePath(line) : line;

            // 颜色判定(保留原控制台语义；优先级见下)。
            ConsoleColor color = classifyColor(line, inTbThisLine);

            // 错误缓冲判定。
            bool isErrorLine  = containsIgnoreCase(line, "[ERROR]") || containsIgnoreCase(line, "[FATAL]");
            bool toErrBuffer  = isErrorLine;

            // traceback 状态机。
            if (startsTraceback) {
                inTraceback_ = true;
                toErrBuffer  = true;
            } else if (inTraceback_) {
                if (isBlank(classify)) {
                    // 未检测到终止异常时，空行结束当前 traceback 块(空行本身不入错误缓冲)。
                    inTraceback_ = false;
                } else {
                    toErrBuffer = true;
                    if (isTracebackTerminator(classify)) {
                        inTraceback_ = false; // 终止异常行属于本块，入缓冲后退出状态。
                    }
                }
            }

            // 4. 控制台输出。
            if (consoleSink_) {
                consoleSink_(display, color);
            }
            // 5. 所有显示行进入普通 logBuffer。
            if (enableBuffering_ && logBuffer_) {
                logBuffer_->add(display);
            }
            // 6. 错误行或 traceback 块进入 errBuffer。
            if (enableBuffering_ && errBuffer_ && toErrBuffer) {
                errBuffer_->add(display);
            }
        }

        // 颜色优先级：Developer > SUC > ERROR/FATAL/traceback > WARN > DEBUG > 默认。
        static ConsoleColor classifyColor(const std::string& line, bool inTraceback) {
            if (line.find("[INFO][Developer]") != std::string::npos) {
                return ConsoleColor::DarkGray;
            }
            if (containsIgnoreCase(line, "SUC")) {
                return ConsoleColor::Green;
            }
            if (inTraceback || containsIgnoreCase(line, "ERROR") || containsIgnoreCase(line, "FATAL")) {
                return ConsoleColor::Red;
            }
            if (containsIgnoreCase(line, "WARN")) {
                return ConsoleColor::Yellow;
            }
            if (containsIgnoreCase(line, "DEBUG")) {
                return ConsoleColor::Cyan;
            }
            return ConsoleColor::Default;
        }

        // 去掉一个前导 "[Python] " 前缀(若存在)，返回视图(不复制)。
        static std::string_view stripPythonPrefix(const std::string& line) {
            constexpr std::string_view kPrefix = "[Python] ";
            std::string_view           v(line);
            if (v.size() >= kPrefix.size() && v.substr(0, kPrefix.size()) == kPrefix) {
                return v.substr(kPrefix.size());
            }
            return v;
        }

        static std::string_view lstrip(std::string_view v) {
            size_t i = 0;
            while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) {
                ++i;
            }
            return v.substr(i);
        }

        static bool isBlank(std::string_view v) {
            for (char c : v) {
                if (c != ' ' && c != '\t' && c != '\r') {
                    return false;
                }
            }
            return true;
        }

        // 终止异常行：去前缀去缩进后形如 Identifier(.Identifier)*(Error|Exception|Interrupt) 后跟 ':' 或行尾。
        // 例如 ValueError: x / KeyboardInterrupt / my.module.FooError: bar。
        static bool isTracebackTerminator(std::string_view classifyView) {
            std::string_view s = lstrip(classifyView);
            if (s.empty()) {
                return false;
            }
            static const std::regex re(
                R"(^[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)*(Error|Exception|Interrupt)(:.*)?$)"
            );
            return std::regex_match(s.begin(), s.end(), re);
        }

        // File "a.b.c", line N  ->  File "a/b/c.py", line N(沿用旧 stderr 转换)。
        static std::string convertDottedFilePath(const std::string& line) {
            static const std::regex fileRe(R"(File \"([A-Za-z0-9_\.]+)\", line (\d+))");
            std::string             out;
            out.reserve(line.size());

            std::sregex_iterator cur(line.begin(), line.end(), fileRe);
            std::sregex_iterator end;
            size_t               lastPos = 0;

            for (; cur != end; ++cur) {
                const std::smatch& m = *cur;
                out.append(line, lastPos, static_cast<size_t>(m.position()) - lastPos);

                std::string dotted  = m[1].str();
                std::string slashed = dotted;
                std::replace(slashed.begin(), slashed.end(), '.', '/');
                slashed += ".py";

                out += "File \"" + slashed + "\", line " + m[2].str();
                lastPos = static_cast<size_t>(m.position()) + static_cast<size_t>(m.length());
            }
            out.append(line, lastPos, std::string::npos);
            return out;
        }

        bool                       filterPython_   = true;
        bool                       enableBuffering_ = true;
        std::shared_ptr<LogBuffer> logBuffer_;
        std::shared_ptr<LogBuffer> errBuffer_;
        ConsoleSink                consoleSink_;

        std::mutex  mutex_;
        std::string residual_;          // 跨 payload 的半行残留。
        bool        inTraceback_ = false; // traceback 块状态。
    };

} // namespace mcdk
