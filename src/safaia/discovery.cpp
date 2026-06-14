#include "mcdevtool/safaia/discovery.h"
#include "mcdevtool/safaia/protocol.h"

#include <chrono>
#include <set>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

namespace MCDevTool::Safaia {

#ifdef _WIN32

    std::vector<std::string> enumerateLocalIPv4() {
        std::vector<std::string> result;
        std::set<std::string>    seen;

        auto add = [&](const std::string& ip) {
            if (!ip.empty() && seen.insert(ip).second) {
                result.push_back(ip);
            }
        };
        add("127.0.0.1");

        ULONG  bufLen   = 15 * 1024;
        BYTE*  buffer   = nullptr;
        DWORD  ret      = 0;
        for (int attempt = 0; attempt < 3; ++attempt) {
            buffer = reinterpret_cast<BYTE*>(malloc(bufLen));
            if (!buffer) {
                return result;
            }
            ret = GetAdaptersAddresses(
                AF_INET,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                nullptr,
                reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer),
                &bufLen
            );
            if (ret == ERROR_BUFFER_OVERFLOW) {
                free(buffer);
                buffer = nullptr;
                continue; // bufLen 已被更新，重试
            }
            break;
        }

        if (ret == NO_ERROR && buffer) {
            for (auto* ad = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer); ad; ad = ad->Next) {
                for (auto* ua = ad->FirstUnicastAddress; ua; ua = ua->Next) {
                    if (ua->Address.lpSockaddr && ua->Address.lpSockaddr->sa_family == AF_INET) {
                        auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                        char  buf[INET_ADDRSTRLEN] = {0};
                        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
                            add(buf);
                        }
                    }
                }
            }
        }
        if (buffer) {
            free(buffer);
        }
        return result;
    }

    DiscoverySender::~DiscoverySender() { stop(); }

    void DiscoverySender::configure(
        std::string advertiseIp, int advertisePort, std::vector<std::string> targetIps, int intervalMs
    ) {
        advertiseIp_   = std::move(advertiseIp);
        advertisePort_ = advertisePort;
        targetIps_     = std::move(targetIps);
        intervalMs_    = intervalMs > 0 ? intervalMs : 500;
    }

    void DiscoverySender::start() {
        if (active_.load()) {
            return;
        }
        // 假定 WSAStartup 已由控制器/IPC 层初始化（同进程内 WSAStartup 计数）。
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            return;
        }
        BOOL on = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
        sockPtr_ = reinterpret_cast<void*>(sock);

        active_.store(true);
        thread_.emplace([this]() { run(); });
    }

    void DiscoverySender::run() {
        const std::string payload = buildDiscoveryPayload(advertiseIp_, advertisePort_);
        SOCKET            sock     = reinterpret_cast<SOCKET>(sockPtr_);

        while (active_.load()) {
            for (const auto& ip : targetIps_) {
                sockaddr_in dst{};
                dst.sin_family = AF_INET;
                if (inet_pton(AF_INET, ip.c_str(), &dst.sin_addr) != 1) {
                    continue;
                }
                for (int i = 0; i < ConstPort::client_listen_count; ++i) {
                    dst.sin_port = htons(static_cast<u_short>(ConstPort::client_listen_base + i));
                    sendto(
                        sock,
                        payload.data(),
                        static_cast<int>(payload.size()),
                        0,
                        reinterpret_cast<sockaddr*>(&dst),
                        sizeof(dst)
                    );
                }
            }
            sentCount_.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs_));
        }
    }

    void DiscoverySender::stop() {
        if (!active_.exchange(false)) {
            // 可能 socket 已创建但线程未起；仍尝试清理。
        }
        if (thread_.has_value()) {
            if (thread_->joinable()) {
                thread_->join();
            }
            thread_.reset();
        }
        if (sockPtr_) {
            closesocket(reinterpret_cast<SOCKET>(sockPtr_));
            sockPtr_ = nullptr;
            WSACleanup();
        }
    }

#else // 非 Windows：可移植桩，保证 mcdevtool 库可链接（不会在非 Windows 路径被实际使用）。

    std::vector<std::string> enumerateLocalIPv4() { return {"127.0.0.1"}; }

    DiscoverySender::~DiscoverySender() {}
    void DiscoverySender::configure(std::string advertiseIp, int advertisePort, std::vector<std::string> targetIps, int intervalMs) {
        advertiseIp_   = std::move(advertiseIp);
        advertisePort_ = advertisePort;
        targetIps_     = std::move(targetIps);
        intervalMs_    = intervalMs;
    }
    void DiscoverySender::start() {}
    void DiscoverySender::run() {}
    void DiscoverySender::stop() {}

#endif

} // namespace MCDevTool::Safaia
