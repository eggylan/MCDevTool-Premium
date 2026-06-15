#pragma once
// Safaia 设备发现：枚举本机 IPv4 + 向游戏侧 UDP 监听端口(26613..26622)周期广播 {ip,port}。
// 对照 temp/safaia_live_probe/discovery.py。Winsock 实现仅 Windows，其余平台为桩。
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <optional>

namespace MCDevTool::Safaia {

    // 枚举本机所有 IPv4 地址（含 127.0.0.1，去重）。非 Windows 返回 {"127.0.0.1"}。
    std::vector<std::string> enumerateLocalIPv4();

    // 查询指定进程当前拥有(owner PID == pid)的 Safaia 设备发现 UDP 端口集合，
    // 仅返回落在 26613..26622 范围内的本地端口(去重、升序)。
    // 基于 Windows GetExtendedUdpTable(UDP_TABLE_OWNER_PID)；非 Windows 返回空。
    // 用于多实例隔离：确保 mcdk 只向自己启动的 Minecraft 的 UDP 端口发送 discovery，
    // 以及在握手时校验 connect_port 归属。
    std::vector<uint16_t> findSafaiaUdpPortsForProcess(uint32_t pid);

    // UDP 设备发现广播器：周期性向 targetIps 的 26613..26622 端口发送
    // {"ip":advertiseIp,"port":advertisePort}，告诉游戏回连地址。
    //
    // 多实例隔离：设置 targetPid 后，每个发送周期通过 findSafaiaUdpPortsForProcess(pid)
    // 解析目标 Minecraft 当前拥有的 26613..26622 端口集合，仅向这些端口定向发送；
    // 未设置 pid(=0) 时退回旧行为(向全部 26613..26622 广播)，兼容尚未接入 PID 的调用方。
    class DiscoverySender {
    public:
        // 内部诊断日志回调：level 为 "info"/"warn"/"error"。
        using LogFn = std::function<void(const std::string& level, const std::string& msg)>;

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

        // 目标 Minecraft PID：设置后启用按 PID 定向端口发送。0 表示退回全端口广播。
        void setTargetPid(uint32_t pid) { targetPid_.store(pid); }
        // 诊断日志回调(端口长时间未出现等)。
        void setLogger(LogFn fn) { logFn_ = std::move(fn); }
        // 暂停/恢复发送：握手成功后暂停，断连后恢复。线程仍存活。
        void setPaused(bool paused) { paused_.store(paused); }

        void     start();
        void     stop();
        bool     isRunning() const { return active_.load(); }
        uint64_t sentCount() const { return sentCount_.load(); }

    private:
        void run();
        void log(const std::string& level, const std::string& msg) {
            if (logFn_) {
                logFn_(level, msg);
            }
        }

        std::string                advertiseIp_;
        int                        advertisePort_ = 0;
        std::vector<std::string>   targetIps_;
        int                        intervalMs_ = 500;
        std::atomic<bool>          active_{false};
        std::atomic<bool>          paused_{false};
        std::atomic<uint32_t>      targetPid_{0};
        LogFn                      logFn_;
        std::optional<std::thread> thread_;
        std::atomic<uint64_t>      sentCount_{0};
        void*                      sockPtr_ = nullptr; // SOCKET（Windows）
    };

} // namespace MCDevTool::Safaia
