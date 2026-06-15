// findSafaiaUdpPortsForProcess 测试(计划 §6.2)。
//
// 父进程派生两个辅助子进程(本 exe 加 --bind 参数)，各自绑定 26613..26622 中指定的 UDP 端口；
// 父进程按子进程 PID 查询，断言：
//   - 只返回该子进程拥有的端口；
//   - 两个子进程的查询结果互不包含；
//   - 子进程退出后查询结果最终清空。
//
// 仅 Windows。子进程绑定失败(端口被占用)时以退出码 2 结束，父进程据此 SKIP 而非误报失败。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <mcdevtool/safaia/discovery.h>

#pragma comment(lib, "ws2_32.lib")

using MCDevTool::Safaia::findSafaiaUdpPortsForProcess;

static int g_failures = 0;
#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << "  CHECK(" #cond ")\n"; \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

// ── 子进程模式：绑定给定端口并保活 ──
static int runChildBind(const std::vector<uint16_t>& ports) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 3;
    }
    std::vector<SOCKET> socks;
    for (uint16_t port : ports) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) {
            return 2;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            return 2; // 端口被占用：让父进程 SKIP
        }
        socks.push_back(s);
    }
    // 保活足够长，等父进程完成查询；父进程会主动 Terminate。
    std::this_thread::sleep_for(std::chrono::seconds(20));
    for (SOCKET s : socks) {
        closesocket(s);
    }
    WSACleanup();
    return 0;
}

static std::wstring selfExePath() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf);
}

// 启动一个绑定指定端口的子进程，返回其 PROCESS_INFORMATION(失败 hProcess=nullptr)。
static PROCESS_INFORMATION spawnChild(const std::vector<uint16_t>& ports) {
    std::wstring cmd = L"\"" + selfExePath() + L"\" --bind";
    for (uint16_t p : ports) {
        cmd += L" " + std::to_wstring(p);
    }
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

static bool hasPort(const std::vector<uint16_t>& v, uint16_t p) {
    return std::find(v.begin(), v.end(), p) != v.end();
}

int main(int argc, char** argv) {
    // 子进程分支。
    if (argc >= 2 && std::string(argv[1]) == "--bind") {
        std::vector<uint16_t> ports;
        for (int i = 2; i < argc; ++i) {
            ports.push_back(static_cast<uint16_t>(std::stoi(argv[i])));
        }
        return runChildBind(ports);
    }

    // ── 父进程 ──
    const std::vector<uint16_t> portsA = {26614, 26615};
    const std::vector<uint16_t> portsB = {26617};

    PROCESS_INFORMATION a = spawnChild(portsA);
    PROCESS_INFORMATION b = spawnChild(portsB);
    if (!a.hProcess || !b.hProcess) {
        std::cerr << "pid_port_query_test: spawn failed\n";
        return 1;
    }

    // 等待子进程完成 bind。
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    if (!stillAlive(a.hProcess) || !stillAlive(b.hProcess)) {
        std::cout << "pid_port_query_test: SKIP (helper port busy or child exited early)\n";
        TerminateProcess(a.hProcess, 0);
        TerminateProcess(b.hProcess, 0);
        return 0;
    }

    std::vector<uint16_t> qa = findSafaiaUdpPortsForProcess(a.dwProcessId);
    std::vector<uint16_t> qb = findSafaiaUdpPortsForProcess(b.dwProcessId);

    // 只返回各自拥有的端口。
    CHECK(hasPort(qa, 26614));
    CHECK(hasPort(qa, 26615));
    CHECK(qa.size() == 2);

    CHECK(hasPort(qb, 26617));
    CHECK(qb.size() == 1);

    // 互不包含。
    CHECK(!hasPort(qa, 26617));
    CHECK(!hasPort(qb, 26614));
    CHECK(!hasPort(qb, 26615));

    // 子进程退出后查询最终清空。
    TerminateProcess(a.hProcess, 0);
    WaitForSingleObject(a.hProcess, 3000);
    bool cleared = false;
    for (int i = 0; i < 20; ++i) { // 最多约 2s 等待内核释放端口归属
        if (findSafaiaUdpPortsForProcess(a.dwProcessId).empty()) {
            cleared = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(cleared);

    TerminateProcess(b.hProcess, 0);
    WaitForSingleObject(b.hProcess, 3000);
    CloseHandle(a.hProcess);
    CloseHandle(a.hThread);
    CloseHandle(b.hProcess);
    CloseHandle(b.hThread);

    if (g_failures == 0) {
        std::cout << "pid_port_query_test: ALL PASS\n";
        return 0;
    }
    std::cerr << "pid_port_query_test: " << g_failures << " FAILURE(S)\n";
    return 1;
}
