#pragma once
#include <atomic>
#include <fstream>
#include <mutex>
#include <string>

// Ordered by severity so a minimum level can filter messages.
enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR
};

class Logger {
private:
    static std::mutex log_mutex;
    static std::ofstream log_file;
    static std::atomic<LogLevel> min_level;
    static bool use_color;
    static std::string get_current_time();
    static std::string level_to_string(LogLevel level);
    static std::string level_to_color(LogLevel level);
    static const std::string RESET_COLOR;

public:
    static void init(const std::string& filepath = "vortex_server.log");
    static void set_level(LogLevel level) { min_level.store(level, std::memory_order_relaxed); }
    // Cheap check so callers can skip building messages that would be dropped.
    static bool enabled(LogLevel level) { return level >= min_level.load(std::memory_order_relaxed); }

    // Core logging function
    static void log(LogLevel level, const std::string& message);
    // Quick-access wrappers
    static void info(const std::string& message) { log(LogLevel::INFO, message); }
    static void warn(const std::string& message) { log(LogLevel::WARNING, message); }
    static void error(const std::string& message) { log(LogLevel::ERROR, message); }
    static void debug(const std::string& message) { log(LogLevel::DEBUG, message); }
};
