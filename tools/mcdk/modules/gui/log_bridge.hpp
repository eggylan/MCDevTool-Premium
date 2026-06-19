#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include <QObject>
#include <QString>

#include "log_level.hpp"

namespace mcdk::gui {

    class LogBridge : public QObject {
        Q_OBJECT

    public:
        explicit LogBridge(QObject* parent = nullptr);

        void                  publish(const std::string& line, ConsoleColor color);
        std::vector<LogEntry> snapshot() const;

    Q_SIGNALS:
        void lineArrived(QString line, int level);

    private:
        static constexpr std::size_t kMaxBufferedEntries = 1000;

        mutable std::mutex  mutex_;
        std::vector<LogEntry> entries_;
    };

} // namespace mcdk::gui
