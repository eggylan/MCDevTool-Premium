#pragma once
// UI 调试进程内共享状态：当前屏、树缓存、最后选中、事件流、就绪标志。
// 由控制器读线程写入，由 MCP 工具线程读取，全部加锁；选中事件提供阻塞等待。
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <nlohmann/json.hpp>

namespace MCDevTool::Safaia {

    // 一条带外 UI 事件（主要是 handle=4 ScreenChanged / handle=7 ControlSelectionChanged）。
    struct UiEvent {
        int            handle = -1;
        std::string    handleName;
        bool           success = false;
        nlohmann::json data;
        int64_t        recvMs = 0; // steady_clock 毫秒，用于排序/去重
        uint64_t       seq    = 0;
    };

    class UiDebugState {
    public:
        // ── 连接 / 就绪 ──
        void setConnected(bool v);
        bool isConnected() const;
        void setReady(bool v);
        bool isReady() const;

        // ── 当前屏（由 ScreenChanged 更新）──
        void        setCurrentScreen(const std::string& name);
        std::string currentScreen() const;

        // ── 树缓存（按 rootPath；ScreenChanged 时整体失效）──
        void                          cacheTree(const std::string& rootPath, const nlohmann::json& tree);
        std::optional<nlohmann::json> getCachedTree(const std::string& rootPath) const;
        void                          invalidateTrees();

        // ── 选中（ControlSelectionChanged，带去重）──
        // 返回 true 表示这是一个新的（非短时窗重复）选中事件，并已 bump seq / 唤醒等待者。
        bool                     updateSelection(const nlohmann::json& data);
        std::vector<std::string> lastSelection() const;
        uint64_t                 selectionSeq() const;
        // 阻塞等待 selectionSeq 超过 sinceSeq 的新选中事件；超时返回 false。
        bool waitForSelection(uint64_t sinceSeq, int timeoutMs, std::vector<std::string>& outPaths, uint64_t& outSeq);

        // ── 事件日志（环形，记录全部带外事件）──
        void                 appendEvent(const UiEvent& ev);
        std::vector<UiEvent> recentEvents(size_t maxCount) const;
        uint64_t             nextEventSeq();

        // ── 断连复位（保留 eventLog，可选）──
        void resetForDisconnect();

        void setDedupWindowMs(int ms);

        // 从选中事件 data 提取控件路径数组（data 为字符串数组或含 path 的对象数组）。
        static std::vector<std::string> extractSelectionPaths(const nlohmann::json& data);

    private:
        mutable std::mutex      mtx_;
        std::condition_variable selCv_;

        bool        connected_ = false;
        bool        ready_     = false;
        std::string currentScreen_;

        std::map<std::string, nlohmann::json> treeCache_;

        std::vector<std::string> lastSelection_;
        uint64_t                 selectionSeq_ = 0;
        std::string              lastSelSig_; // 去重签名（路径 join）
        int64_t                  lastSelMs_   = 0;
        int                      dedupWindowMs_ = 300; // 真机 ~3 帧突发去重窗

        std::deque<UiEvent> eventLog_;
        size_t              eventLogCap_ = 500;
        uint64_t            eventSeq_    = 0;
    };

} // namespace MCDevTool::Safaia
