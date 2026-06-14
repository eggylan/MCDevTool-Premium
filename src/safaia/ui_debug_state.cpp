#include "mcdevtool/safaia/ui_debug_state.h"
#include <chrono>

namespace MCDevTool::Safaia {

    namespace {
        int64_t nowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()
            )
                .count();
        }
    } // namespace

    void UiDebugState::setConnected(bool v) {
        std::lock_guard<std::mutex> lk(mtx_);
        connected_ = v;
    }

    bool UiDebugState::isConnected() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return connected_;
    }

    void UiDebugState::setReady(bool v) {
        std::lock_guard<std::mutex> lk(mtx_);
        ready_ = v;
    }

    bool UiDebugState::isReady() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return ready_;
    }

    void UiDebugState::setCurrentScreen(const std::string& name) {
        std::lock_guard<std::mutex> lk(mtx_);
        currentScreen_ = name;
    }

    std::string UiDebugState::currentScreen() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return currentScreen_;
    }

    void UiDebugState::cacheTree(const std::string& rootPath, const nlohmann::json& tree) {
        std::lock_guard<std::mutex> lk(mtx_);
        treeCache_[rootPath] = tree;
    }

    std::optional<nlohmann::json> UiDebugState::getCachedTree(const std::string& rootPath) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto                        it = treeCache_.find(rootPath);
        if (it == treeCache_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void UiDebugState::invalidateTrees() {
        std::lock_guard<std::mutex> lk(mtx_);
        treeCache_.clear();
    }

    std::vector<std::string> UiDebugState::extractSelectionPaths(const nlohmann::json& data) {
        std::vector<std::string> paths;
        if (data.is_array()) {
            for (const auto& el : data) {
                if (el.is_string()) {
                    paths.push_back(el.get<std::string>());
                } else if (el.is_object() && el.contains("path") && el["path"].is_string()) {
                    paths.push_back(el["path"].get<std::string>());
                }
            }
        } else if (data.is_string()) {
            paths.push_back(data.get<std::string>());
        } else if (data.is_object() && data.contains("path") && data["path"].is_string()) {
            paths.push_back(data["path"].get<std::string>());
        }
        return paths;
    }

    bool UiDebugState::updateSelection(const nlohmann::json& data) {
        auto paths = extractSelectionPaths(data);

        std::string sig;
        for (const auto& p : paths) {
            sig += p;
            sig += '\n';
        }

        std::lock_guard<std::mutex> lk(mtx_);
        int64_t                     t = nowMs();
        // 短时窗内相同选中视为同一突发的重复帧（真机一次单击连推 ~3 帧）。
        if (sig == lastSelSig_ && (t - lastSelMs_) <= dedupWindowMs_) {
            lastSelMs_ = t; // 刷新时间，但不算新事件
            return false;
        }
        lastSelSig_    = sig;
        lastSelMs_     = t;
        lastSelection_ = std::move(paths);
        ++selectionSeq_;
        selCv_.notify_all();
        return true;
    }

    std::vector<std::string> UiDebugState::lastSelection() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return lastSelection_;
    }

    uint64_t UiDebugState::selectionSeq() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return selectionSeq_;
    }

    bool UiDebugState::waitForSelection(
        uint64_t sinceSeq, int timeoutMs, std::vector<std::string>& outPaths, uint64_t& outSeq
    ) {
        std::unique_lock<std::mutex> lk(mtx_);
        bool                         got = selCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] {
            return selectionSeq_ > sinceSeq;
        });
        if (!got) {
            return false;
        }
        outPaths = lastSelection_;
        outSeq   = selectionSeq_;
        return true;
    }

    uint64_t UiDebugState::nextEventSeq() {
        std::lock_guard<std::mutex> lk(mtx_);
        return ++eventSeq_;
    }

    void UiDebugState::appendEvent(const UiEvent& ev) {
        std::lock_guard<std::mutex> lk(mtx_);
        eventLog_.push_back(ev);
        while (eventLog_.size() > eventLogCap_) {
            eventLog_.pop_front();
        }
    }

    std::vector<UiEvent> UiDebugState::recentEvents(size_t maxCount) const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<UiEvent>        out;
        size_t                      n = eventLog_.size();
        size_t                      start = (maxCount >= n) ? 0 : (n - maxCount);
        for (size_t i = start; i < n; ++i) {
            out.push_back(eventLog_[i]);
        }
        return out;
    }

    void UiDebugState::resetForDisconnect() {
        std::lock_guard<std::mutex> lk(mtx_);
        connected_ = false;
        ready_     = false;
        currentScreen_.clear();
        treeCache_.clear();
        // 保留 lastSelection_ / eventLog_ 作为历史；不清空。
    }

    void UiDebugState::setDedupWindowMs(int ms) {
        std::lock_guard<std::mutex> lk(mtx_);
        dedupWindowMs_ = ms;
    }

} // namespace MCDevTool::Safaia
