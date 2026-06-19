#pragma once

#include <QString>

#include "../console.hpp"

namespace mcdk::gui {

    enum class LogLevel {
        Normal = 0,
        Error,
        Warning,
        Debug,
        Success,
        Developer,
    };

    struct LogEntry {
        QString  line;
        LogLevel level = LogLevel::Normal;
    };

    inline LogLevel logLevelFromConsoleColor(ConsoleColor color) {
        switch (color) {
        case ConsoleColor::Red:
            return LogLevel::Error;
        case ConsoleColor::Yellow:
            return LogLevel::Warning;
        case ConsoleColor::Cyan:
            return LogLevel::Debug;
        case ConsoleColor::Green:
            return LogLevel::Success;
        case ConsoleColor::DarkGray:
            return LogLevel::Developer;
        default:
            return LogLevel::Normal;
        }
    }

    inline LogLevel classifyLogLine(const QString& line, LogLevel fallback = LogLevel::Normal) {
        if (line.contains(QStringLiteral("[INFO][Developer]"))) {
            return LogLevel::Developer;
        }
        if (line.contains(QStringLiteral("SUC"), Qt::CaseInsensitive)) {
            return LogLevel::Success;
        }
        if (line.contains(QStringLiteral("Traceback (most recent call last):"))
            || line.contains(QStringLiteral("ERROR"), Qt::CaseInsensitive)
            || line.contains(QStringLiteral("FATAL"), Qt::CaseInsensitive)) {
            return LogLevel::Error;
        }
        if (line.contains(QStringLiteral("WARN"), Qt::CaseInsensitive)) {
            return LogLevel::Warning;
        }
        if (line.contains(QStringLiteral("DEBUG"), Qt::CaseInsensitive)) {
            return LogLevel::Debug;
        }
        return fallback;
    }

} // namespace mcdk::gui
