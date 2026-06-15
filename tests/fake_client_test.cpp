// SafaiaController fake-client 多实例隔离测试(计划 §6.3)。
//
// 在同一进程内运行两个 SafaiaController(各自 setMinecraftPid 指向一个 fake-Minecraft 辅助进程)，
// 辅助进程绑定不同 Safaia UDP 端口、接收对应 controller 的 discovery、回连各自 TCP server，
// 用正确 connect_port 握手成功并发送唯一协议 4 日志标记 + 一个 cmd=7 选中事件。
//
// 断言：
//   1. 日志标记只进入对应 controller 的 message handler，不串台；
//   2. cmd=7(带外选中事件)仍由原 controller 路由(selectionSeq 增长)，不被日志处理影响；
//   3. 错误 connect_port(不属于目标 PID)被拒绝(connect_block，不置连接、不路由日志)；
//   4. 断连后 PID 定向 discovery 恢复，目标实例可重连(同 PID)。
//
// 仅 Windows。辅助进程绑定端口失败(被占用)时退出码 2，父进程 SKIP 相关子测试。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <nlohmann/json.hpp>
#include <mcdevtool/safaia/controller.h>
#include <mcdevtool/safaia/discovery.h>
#include <mcdevtool/safaia/protocol.h>

#pragma comment(lib, "ws2_32.lib")

using namespace MCDevTool::Safaia;

static int g_failures = 0;
#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << "  CHECK(" #cond ")\n"; \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

// ───────────────────────── 辅助进程(fake Minecraft) ─────────────────────────

static bool sendAll(SOCKET sock, const std::vector<uint8_t>& bytes) {
    size_t sent = 0;
    while (sent < bytes.size()) {
        int n = send(sock, reinterpret_cast<const char*>(bytes.data() + sent),
                     static_cast<int>(bytes.size() - sent), 0);
        if (n == SOCKET_ERROR) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

static bool sendFrame(SOCKET sock, int32_t proto, const nlohmann::json& payload) {
    return sendAll(sock, ProtocolFramer::packJson(proto, payload));
}

// 等待并接收一个指定协议号集合内的帧；超时返回 -1，否则返回收到的 protocolId。
static int waitFrame(SOCKET sock, ProtocolFramer& framer, std::vector<Frame>& pending, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        if (!pending.empty()) {
            int p = pending.front().protocolId;
            pending.erase(pending.begin());
            return p;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return -1;
        }
        DWORD to = 100;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
        uint8_t buf[2048];
        int     n = recv(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n > 0) {
            auto frames = framer.input(buf, static_cast<size_t>(n));
            for (auto& f : frames) {
                pending.push_back(std::move(f));
            }
        } else if (n == 0) {
            return -2; // 对端关闭
        }
    }
}

// 建立一次 UDP 发现 -> TCP 回连 -> 握手 -> 发标记 + cmd=7 的连接。
// claimPort：握手时声明的 connect_port(默认=udpPort，传别的值用于测试拒绝)。
// 返回 TCP socket(已连接且握手成功)，失败返回 INVALID_SOCKET。
static SOCKET doConnectCycle(SOCKET udpSock, uint16_t udpPort, int claimPort, const std::string& marker,
                             bool sendSelection, bool& rejected) {
    rejected = false;

    // 1. 等 discovery，拿到 controller 的 TCP ip:port。
    char        dgram[1024];
    sockaddr_in from{};
    int         fromLen = sizeof(from);
    DWORD       to      = 6000;
    setsockopt(udpSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    int n = recvfrom(udpSock, dgram, sizeof(dgram) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (n <= 0) {
        return INVALID_SOCKET;
    }
    dgram[n]   = '\0';
    auto disco = nlohmann::json::parse(std::string(dgram, n), nullptr, false);
    if (disco.is_discarded() || !disco.is_object()) {
        return INVALID_SOCKET;
    }
    std::string ip   = disco.value("ip", "127.0.0.1");
    int         port = 0;
    if (disco.contains("port")) {
        port = disco["port"].is_number_integer() ? disco["port"].get<int>()
                                                  : std::stoi(disco.value("port", std::string("0")));
    }
    if (port == 0) {
        return INVALID_SOCKET;
    }

    // 2. TCP 回连。
    SOCKET tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcp == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, ip.c_str(), &srv.sin_addr);
    if (connect(tcp, reinterpret_cast<sockaddr*>(&srv), sizeof(srv)) == SOCKET_ERROR) {
        closesocket(tcp);
        return INVALID_SOCKET;
    }

    // 3. 发 config 握手。
    nlohmann::json cfg{
        {"connect_id", "fake-" + marker},
        {"connect_port", claimPort},
        {"name", "fakemc-" + marker},
        {"u_id", 1},
        {"platform", 0},
    };
    sendFrame(tcp, MCProtocol::config, cfg);

    // 4. 等 connect_success / connect_block。
    ProtocolFramer     framer;
    std::vector<Frame> pending;
    int                p = waitFrame(tcp, framer, pending, 4000);
    if (p == MCProtocol::connect_block) {
        rejected = true;
        closesocket(tcp);
        return INVALID_SOCKET;
    }
    if (p != MCProtocol::connect_success) {
        closesocket(tcp);
        return INVALID_SOCKET;
    }

    // 5. 发协议 4 日志标记(原始文本字节，贴近真机：payload 不是 JSON 而是日志文本)。
    sendAll(tcp, ProtocolFramer::pack(MCProtocol::message, "[Python] RAW_" + marker + "\n"));

    // 6. 发一个 cmd=7 带外选中事件(handle=7 ControlSelectionChanged)。
    if (sendSelection) {
        nlohmann::json inner{
            {"handle", RPCHandles::ControlSelectionChanged},
            {"success", true},
            {"data", nlohmann::json::array({"/Foo/" + marker})},
        };
        nlohmann::json outer{
            {"cmd", "uidebuger_call"},
            {"result", inner.dump()}, // 真机形态：result 为 JSON 字符串
        };
        sendFrame(tcp, MCProtocol::cmd, outer);
    }

    return tcp;
}

static int runFakeMc(uint16_t udpPort, const std::string& marker, int cycles) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 3;
    }
    SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp == INVALID_SOCKET) {
        return 3;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(udpPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(udp, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        return 2; // 端口占用 -> 父进程 SKIP
    }

    for (int c = 0; c < cycles; ++c) {
        bool   rejected = false;
        SOCKET tcp      = doConnectCycle(udp, udpPort, udpPort, marker + std::to_string(c), c == 0, rejected);
        if (tcp == INVALID_SOCKET) {
            continue;
        }
        // 持有连接一段时间(让父进程完成断言)，再主动断开以触发 discovery 恢复。
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        closesocket(tcp);
        // 进程(及 UDP 端口归属)保持存活，等待下一轮 discovery 恢复后重连。
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    closesocket(udp);
    WSACleanup();
    return 0;
}

static std::wstring selfExePath() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf);
}

static PROCESS_INFORMATION spawnFake(uint16_t udpPort, const std::string& marker, int cycles) {
    std::wstring cmd = L"\"" + selfExePath() + L"\" --fake-mc " + std::to_wstring(udpPort) + L" "
                     + std::wstring(marker.begin(), marker.end()) + L" " + std::to_wstring(cycles);
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW        si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        pi.hProcess = nullptr;
    }
    return pi;
}

static bool stillAlive(HANDLE h) {
    DWORD code = 0;
    return GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
}

// 构造一个 controller，targetIps 固定 127.0.0.1，快速 discovery。
static SafaiaControllerConfig makeCfg() {
    SafaiaControllerConfig cfg;
    cfg.enabled             = true;
    cfg.bindIp              = "127.0.0.1";
    cfg.bindPort            = 0;
    cfg.advertiseIp         = "127.0.0.1";
    cfg.targetIps           = {"127.0.0.1"};
    cfg.discoveryIntervalMs = 150;
    return cfg;
}

template <typename Pred>
static bool waitUntil(Pred pred, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

// ───────────────────────── 子测试 1：隔离 + 路由 + cmd=7 + 恢复 ─────────────────────────
static void testIsolationRoutingResume() {
    const uint16_t portA = 26614, portB = 26617;

    PROCESS_INFORMATION fa = spawnFake(portA, "AAA", 2); // 2 轮：测试断连后重连
    PROCESS_INFORMATION fb = spawnFake(portB, "BBB", 1);
    if (!fa.hProcess || !fb.hProcess) {
        std::cerr << "fake_client_test: spawn failed\n";
        ++g_failures;
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800)); // 等 UDP bind
    if (!stillAlive(fa.hProcess) || !stillAlive(fb.hProcess)) {
        std::cout << "fake_client_test[isolation]: SKIP (helper port busy)\n";
        TerminateProcess(fa.hProcess, 0);
        TerminateProcess(fb.hProcess, 0);
        return;
    }

    std::mutex               mtxA, mtxB;
    std::vector<std::string> logA, logB;

    SafaiaController ctrlA, ctrlB;
    ctrlA.configure(makeCfg());
    ctrlB.configure(makeCfg());
    ctrlA.setMinecraftPid(fa.dwProcessId);
    ctrlB.setMinecraftPid(fb.dwProcessId);
    ctrlA.setMessageHandler([&](std::string_view s) {
        std::lock_guard<std::mutex> lk(mtxA);
        logA.emplace_back(s);
    });
    ctrlB.setMessageHandler([&](std::string_view s) {
        std::lock_guard<std::mutex> lk(mtxB);
        logB.emplace_back(s);
    });
    CHECK(ctrlA.start());
    CHECK(ctrlB.start());

    auto logAHas = [&](const std::string& sub) {
        std::lock_guard<std::mutex> lk(mtxA);
        for (auto& l : logA) {
            if (l.find(sub) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    auto logBHas = [&](const std::string& sub) {
        std::lock_guard<std::mutex> lk(mtxB);
        for (auto& l : logB) {
            if (l.find(sub) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    // 两个 controller 都应握手成功(各自的目标实例)。
    CHECK(waitUntil([&] { return ctrlA.isConnected(); }, 8000));
    CHECK(waitUntil([&] { return ctrlB.isConnected(); }, 8000));

    // 各自的协议 4 标记到达对应 handler。
    CHECK(waitUntil([&] { return logAHas("RAW_AAA0"); }, 5000));
    CHECK(waitUntil([&] { return logBHas("RAW_BBB0"); }, 5000));

    // 不串台。
    CHECK(!logAHas("BBB"));
    CHECK(!logBHas("AAA"));

    // cmd=7 选中事件仍被路由(不被日志处理影响)。
    CHECK(waitUntil([&] { return ctrlA.state().selectionSeq() > 0; }, 5000));
    CHECK(waitUntil([&] { return ctrlB.state().selectionSeq() > 0; }, 5000));

    // 断连后 discovery 恢复：fa 第 0 轮断开后，ctrlA discovery 应恢复发送(端口仍归 fa 所有)。
    CHECK(waitUntil([&] { return !ctrlA.isConnected(); }, 5000)); // fa 关闭第 0 轮 TCP
    uint64_t before = ctrlA.discoverySentCount();
    bool     grew   = waitUntil([&] { return ctrlA.discoverySentCount() > before; }, 3000);
    CHECK(grew); // 恢复定向发送

    // fa 第 1 轮重连成功。
    CHECK(waitUntil([&] { return ctrlA.isConnected(); }, 8000));
    CHECK(waitUntil([&] { return logAHas("RAW_AAA1"); }, 5000));

    ctrlA.stop();
    ctrlB.stop();
    TerminateProcess(fa.hProcess, 0);
    TerminateProcess(fb.hProcess, 0);
    WaitForSingleObject(fa.hProcess, 2000);
    WaitForSingleObject(fb.hProcess, 2000);
    CloseHandle(fa.hProcess);
    CloseHandle(fa.hThread);
    CloseHandle(fb.hProcess);
    CloseHandle(fb.hThread);
}

// ───────────────────────── 子测试 2：错误 connect_port 被拒绝 ─────────────────────────
static void testRejectWrongPort() {
    // controller 目标 PID 设为本测试进程：本进程不拥有任何 26613..26622 端口，
    // 因此任何 connect_port 都不属于目标 PID -> 必然拒绝。
    std::mutex               mtx;
    std::vector<std::string> log;

    SafaiaController ctrl;
    ctrl.configure(makeCfg());
    ctrl.setMinecraftPid(GetCurrentProcessId());
    ctrl.setMessageHandler([&](std::string_view s) {
        std::lock_guard<std::mutex> lk(mtx);
        log.emplace_back(s);
    });
    CHECK(ctrl.start());

    // fake 直接连 controller 的 TCP，声明一个端口(不属于本进程)。
    SOCKET tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(tcp != INVALID_SOCKET);
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(static_cast<u_short>(ctrl.boundPort()));
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);
    bool connectedOk = connect(tcp, reinterpret_cast<sockaddr*>(&srv), sizeof(srv)) != SOCKET_ERROR;
    CHECK(connectedOk);

    nlohmann::json cfg{
        {"connect_id", "bad"},
        {"connect_port", 26620},
        {"name", "bad"},
    };
    sendFrame(tcp, MCProtocol::config, cfg);

    // 应收到 connect_block，且 controller 不进入已连接、不路由日志。
    ProtocolFramer     framer;
    std::vector<Frame> pending;
    int                p = waitFrame(tcp, framer, pending, 4000);
    CHECK(p == MCProtocol::connect_block);

    // 再发一条协议 4，确认不被路由。
    sendAll(tcp, ProtocolFramer::pack(MCProtocol::message, "[Python] RAW_BAD\n"));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    CHECK(!ctrl.isConnected());
    {
        std::lock_guard<std::mutex> lk(mtx);
        bool                        routed = false;
        for (auto& l : log) {
            if (l.find("BAD") != std::string::npos) {
                routed = true;
            }
        }
        CHECK(!routed);
    }

    closesocket(tcp);
    ctrl.stop();
}

int main(int argc, char** argv) {
    if (argc >= 5 && std::string(argv[1]) == "--fake-mc") {
        uint16_t    port   = static_cast<uint16_t>(std::stoi(argv[2]));
        std::string marker = argv[3];
        int         cycles = std::stoi(argv[4]);
        return runFakeMc(port, marker, cycles);
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    testIsolationRoutingResume();
    testRejectWrongPort();

    WSACleanup();

    if (g_failures == 0) {
        std::cout << "fake_client_test: ALL PASS\n";
        return 0;
    }
    std::cerr << "fake_client_test: " << g_failures << " FAILURE(S)\n";
    return 1;
}
