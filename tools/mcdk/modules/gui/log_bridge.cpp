#include "log_bridge.hpp"

namespace mcdk::gui {

    LogBridge::LogBridge(QObject* parent)
    : QObject(parent) {}

    void LogBridge::publish(const std::string& line, ConsoleColor color) {
        LogEntry entry{QString::fromStdString(line), logLevelFromConsoleColor(color)};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entries_.push_back(entry);
            if (entries_.size() > kMaxBufferedEntries) {
                entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(
                    entries_.size() - kMaxBufferedEntries
                ));
            }
        }
        Q_EMIT lineArrived(entry.line, static_cast<int>(entry.level));
    }

    std::vector<LogEntry> LogBridge::snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_;
    }

} // namespace mcdk::gui
