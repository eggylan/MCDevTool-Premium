#pragma once
// Safaia 设备发现：枚举本机 IPv4 + 向游戏侧 UDP 监听端口(26613..26622)周期广播 {ip,port}。
// 对照 temp/safaia_live_probe/discovery.py。Winsock 实现仅 Windows，其余平台为桩。
#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <optional>

namespace MCDevTool::Safaia {

    // 枚举本机所有 IPv4 地址（含 127.0.0.1，去重）。非 Windows 返回 {"127.0.0.1"}。
    std::vector<std::string> enumerateLocalIPv4();

    // UDP 设备发现广播器：周期性向 targetIps 的 26613..26622 端口发送
    // {"ip":advertiseIp,"port":advertisePort}，告诉游戏回连地址。
    class DiscoverySender {
    public:
        DiscoverySender() = default;
        ~DiscoverySender();

        DiscoverySender(const DiscoverySender&)            = delete;
        DiscoverySender& operator=(const DiscoverySender&) = delete;

        // advertiseIp/Port：写入 payload，游戏据此回连（应为 TCP server 的 ip:port）。
        // targetIps：UDP 发送目标（游戏 UDP 监听可能绑定的本机地址候选）。
        void configure(
            std::string              advertiseIp,
            int                      advertisePort,
            std::vector<std::string> targetIps,
            int                      intervalMs = 500
        );
        void     start();
        void     stop();
        bool     isRunning() const { return active_.load(); }
        uint64_t sentCount() const { return sentCount_.load(); }

    private:
        void run();

        std::string                advertiseIp_;
        int                        advertisePort_ = 0;
        std::vector<std::string>   targetIps_;
        int                        intervalMs_ = 500;
        std::atomic<bool>          active_{false};
        std::optional<std::thread> thread_;
        std::atomic<uint64_t>      sentCount_{0};
        void*                      sockPtr_ = nullptr; // SOCKET（Windows）
    };

} // namespace MCDevTool::Safaia
