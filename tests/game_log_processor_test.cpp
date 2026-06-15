// GameLogProcessor 单元测试
// 覆盖：完整行 / 单 payload 多行 / 一行拆多 payload / CRLF / flush 残行 /
// include_debug_mod true&false / Engine INFO 噪音 / 颜色分类 / 全部显示行入普通缓冲 /
// ERROR&FATAL 入错误缓冲 / 多行 traceback 全块入错误缓冲 / traceback 后普通日志不入错误缓冲 /
// dotted module 路径转换。
//
// 无第三方测试框架，使用最小 CHECK 宏；任意失败则返回非零退出码。
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "modules/game_log_processor.hpp"

using mcdk::ConsoleColor;
using mcdk::GameLogProcessor;
using mcdk::LogBuffer;

static int g_failures = 0;

#define CHECK(cond)                                                                                       \
    do {                                                                                                  \
        if (!(cond)) {                                                                                    \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << "  CHECK(" #cond ")\n";              \
            ++g_failures;                                                                                 \
        }                                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                  \
        auto _va = (a);                                                                                   \
        auto _vb = (b);                                                                                   \
        if (!(_va == _vb)) {                                                                              \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << "  CHECK_EQ(" #a ", " #b ")  got ["  \
                      << _va << "] vs [" << _vb << "]\n";                                                 \
            ++g_failures;                                                                                 \
        }                                                                                                 \
    } while (0)

// 测试夹具：捕获控制台输出 + 普通/错误缓冲。
struct Fixture {
    std::shared_ptr<LogBuffer>                       logBuf  = std::make_shared<LogBuffer>(10000, 100);
    std::shared_ptr<LogBuffer>                       errBuf  = std::make_shared<LogBuffer>(10000, 100);
    std::vector<std::pair<std::string, ConsoleColor>> console;
    std::unique_ptr<GameLogProcessor>                proc;

    Fixture(bool filterPython, bool buffering = true) {
        proc = std::make_unique<GameLogProcessor>(
            filterPython,
            logBuf,
            errBuf,
            [this](const std::string& line, ConsoleColor color) { console.emplace_back(line, color); },
            buffering
        );
    }

    std::vector<std::string> log() { return logBuf->getLatest(100000); }
    std::vector<std::string> err() { return errBuf->getLatest(100000); }
};

static bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& e : v) {
        if (e == s) {
            return true;
        }
    }
    return false;
}

int main() {
    // 1. 单个完整日志行。
    {
        Fixture f(false);
        f.proc->consume("hello world\n");
        CHECK_EQ(f.console.size(), 1u);
        CHECK_EQ(f.console[0].first, std::string("hello world"));
        CHECK_EQ(f.log().size(), 1u);
        CHECK(f.err().empty());
    }

    // 2. 一个 payload 中多行。
    {
        Fixture f(false);
        f.proc->consume("a\nb\nc\n");
        CHECK_EQ(f.log().size(), 3u);
        CHECK(contains(f.log(), "a"));
        CHECK(contains(f.log(), "b"));
        CHECK(contains(f.log(), "c"));
    }

    // 3. 一行拆成多个 payload。
    {
        Fixture f(false);
        f.proc->consume("par");
        f.proc->consume("tial ");
        f.proc->consume("line\n");
        CHECK_EQ(f.log().size(), 1u);
        CHECK_EQ(f.log()[0], std::string("partial line"));
    }

    // 4. CRLF。
    {
        Fixture f(false);
        f.proc->consume("crlf line\r\nsecond\r\n");
        CHECK_EQ(f.log().size(), 2u);
        CHECK_EQ(f.log()[0], std::string("crlf line"));
        CHECK_EQ(f.log()[1], std::string("second"));
    }

    // 5. 退出时无换行残行由 flush 处理。
    {
        Fixture f(false);
        f.proc->consume("no newline tail");
        CHECK(f.log().empty()); // 尚未遇到 '\n'
        f.proc->flush();
        CHECK_EQ(f.log().size(), 1u);
        CHECK_EQ(f.log()[0], std::string("no newline tail"));
    }

    // 6. include_debug_mod=true：仅显示含 "[Python] " 的行。
    {
        Fixture f(true);
        f.proc->consume("[Python] kept line\n");
        f.proc->consume("engine internal line\n");
        CHECK_EQ(f.log().size(), 1u);
        CHECK_EQ(f.log()[0], std::string("[Python] kept line"));
    }

    // 6b. include_debug_mod=false：显示全部行。
    {
        Fixture f(false);
        f.proc->consume("[Python] a\n");
        f.proc->consume("plain b\n");
        CHECK_EQ(f.log().size(), 2u);
    }

    // 7. Engine INFO 噪音过滤(即便含 [Python] 也丢弃)。
    {
        Fixture f(false);
        f.proc->consume("x [INFO][Engine] noisy\n");
        f.proc->consume("[Python] y [INFO][Engine] still noisy\n");
        f.proc->consume("normal\n");
        CHECK_EQ(f.log().size(), 1u);
        CHECK_EQ(f.log()[0], std::string("normal"));
    }

    // 8. 颜色分类：Developer / SUC / ERROR / WARN / DEBUG / 默认。
    {
        Fixture f(false);
        f.proc->consume("[INFO][Developer] dev line\n");      // DarkGray
        f.proc->consume("operation SUC done\n");              // Green
        f.proc->consume("[ERROR] boom\n");                    // Red
        f.proc->consume("[WARN] careful\n");                  // Yellow
        f.proc->consume("[DEBUG] trace\n");                   // Cyan
        f.proc->consume("just text\n");                       // Default
        CHECK_EQ(f.console.size(), 6u);
        CHECK(f.console[0].second == ConsoleColor::DarkGray);
        CHECK(f.console[1].second == ConsoleColor::Green);
        CHECK(f.console[2].second == ConsoleColor::Red);
        CHECK(f.console[3].second == ConsoleColor::Yellow);
        CHECK(f.console[4].second == ConsoleColor::Cyan);
        CHECK(f.console[5].second == ConsoleColor::Default);
    }

    // 9. 所有显示行(含彩色分类行)都进入普通缓冲。
    {
        Fixture f(false);
        f.proc->consume("[INFO][Developer] dev\n");
        f.proc->consume("SUC ok\n");
        f.proc->consume("[ERROR] bad\n");
        f.proc->consume("[WARN] w\n");
        f.proc->consume("[DEBUG] d\n");
        f.proc->consume("plain\n");
        CHECK_EQ(f.log().size(), 6u);
    }

    // 10. ERROR / FATAL 同时进入错误缓冲。
    {
        Fixture f(false);
        f.proc->consume("[ERROR] something failed\n");
        f.proc->consume("[FATAL] hard down\n");
        f.proc->consume("[INFO] fine\n");
        CHECK_EQ(f.err().size(), 2u);
        CHECK(contains(f.err(), "[ERROR] something failed"));
        CHECK(contains(f.err(), "[FATAL] hard down"));
        CHECK(!contains(f.err(), "[INFO] fine"));
    }

    // 11. 多行 traceback：从 Traceback 头到终止异常全部进入错误缓冲；终止后普通日志不入错误缓冲。
    {
        Fixture f(false);
        f.proc->consume("Traceback (most recent call last):\n");
        f.proc->consume("  File \"foo.bar.baz\", line 12, in handler\n");
        f.proc->consume("    do_something()\n");
        f.proc->consume("ValueError: invalid value\n");
        f.proc->consume("[INFO] back to normal\n");

        // traceback 块四行全部入错误缓冲。
        CHECK_EQ(f.err().size(), 4u);
        CHECK(contains(f.err(), "Traceback (most recent call last):"));
        CHECK(contains(f.err(), "    do_something()"));
        CHECK(contains(f.err(), "ValueError: invalid value"));
        // 终止后的普通日志不入错误缓冲。
        CHECK(!contains(f.err(), "[INFO] back to normal"));

        // 12. dotted module 路径转换(File "foo.bar.baz" -> "foo/bar/baz.py")。
        CHECK(contains(f.err(), "  File \"foo/bar/baz.py\", line 12, in handler"));
        CHECK(contains(f.log(), "  File \"foo/bar/baz.py\", line 12, in handler"));
    }

    // 11b. 带 [Python] 前缀的 traceback(filterPython=true)同样整块入错误缓冲且能识别终止异常。
    {
        Fixture f(true);
        f.proc->consume("[Python] Traceback (most recent call last):\n");
        f.proc->consume("[Python]   File \"pkg.mod\", line 5, in run\n");
        f.proc->consume("[Python] KeyboardInterrupt\n");
        f.proc->consume("[Python] [INFO] resumed\n");
        CHECK_EQ(f.err().size(), 3u);
        CHECK(contains(f.err(), "[Python] KeyboardInterrupt"));
        CHECK(!contains(f.err(), "[Python] [INFO] resumed"));
        CHECK(contains(f.err(), "[Python]   File \"pkg/mod.py\", line 5, in run"));
    }

    // 11c. 无终止异常时空行结束 traceback 块(filterPython=false)。
    {
        Fixture f(false);
        f.proc->consume("Traceback (most recent call last):\n");
        f.proc->consume("  File \"a.b\", line 1, in x\n");
        f.proc->consume("\n");                 // 空行结束块
        f.proc->consume("after block normal\n");
        // 头 + 一帧 入错误缓冲；空行与其后普通行不入。
        CHECK_EQ(f.err().size(), 2u);
        CHECK(!contains(f.err(), "after block normal"));
    }

    if (g_failures == 0) {
        std::cout << "game_log_processor_test: ALL PASS\n";
        return 0;
    }
    std::cerr << "game_log_processor_test: " << g_failures << " FAILURE(S)\n";
    return 1;
}
