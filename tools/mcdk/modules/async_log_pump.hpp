#pragma once
// 异步日志泵：把原始日志 payload 的处理从「生产者线程」解耦到专用 worker 线程。
//
// 背景(防回归)：游戏日志现在经 Safaia 协议 4 在 controller 的 TCP recv 线程上送达。
// 若直接在该线程上做处理(分行/分类/控制台输出/缓冲)，而控制台写入在 stdout 为慢消费者
// (被 PowerShell 重定向捕获、或真机 Windows 控制台渲染)时会阻塞，就会拖住 recv 线程，
// 使 TCP 接收缓冲填满 -> 反压游戏侧 Safaia send -> 游戏主线程阻塞 -> 界面卡死。
//
// 因此 recv 线程只调用 post()(仅入队，尽快返回)，由本类的 worker 线程串行调用 sink 做实际处理。
// 即便 sink(控制台/磁盘)变慢，也只会让队列暂时增长，绝不阻塞 recv 线程，从而不反压游戏。
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace mcdk {

    class AsyncLogPump {
    public:
        using Sink = std::function<void(std::string_view payload)>;

        explicit AsyncLogPump(Sink sink) : sink_(std::move(sink)) {
            worker_ = std::thread([this]() { run(); });
        }
        ~AsyncLogPump() { stop(); }

        AsyncLogPump(const AsyncLogPump&)            = delete;
        AsyncLogPump& operator=(const AsyncLogPump&) = delete;

        // 生产者线程(Safaia recv 线程)调用：仅入队后立即返回，不做任何可能阻塞的处理。
        void post(std::string_view payload) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (done_) {
                    return;
                }
                queue_.emplace_back(payload.data(), payload.size());
            }
            cv_.notify_one();
        }

        // 停止：处理完队列中已入队的剩余项后退出 worker。幂等，可重复调用。
        void stop() {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (done_) {
                    return;
                }
                done_ = true;
            }
            cv_.notify_all();
            if (worker_.joinable()) {
                worker_.join();
            }
        }

    private:
        void run() {
            for (;;) {
                std::string item;
                {
                    std::unique_lock<std::mutex> lk(mtx_);
                    cv_.wait(lk, [this]() { return !queue_.empty() || done_; });
                    if (queue_.empty()) {
                        if (done_) {
                            return; // 队列已清空且已停止
                        }
                        continue;
                    }
                    item = std::move(queue_.front());
                    queue_.pop_front();
                }
                if (sink_) {
                    sink_(item); // 即便 done_，也先把队列里已入队的项处理完
                }
            }
        }

        Sink                    sink_;
        std::mutex              mtx_;
        std::condition_variable cv_;
        std::deque<std::string> queue_;
        bool                    done_ = false;
        std::thread             worker_;
    };

} // namespace mcdk
