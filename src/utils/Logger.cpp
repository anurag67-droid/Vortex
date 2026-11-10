#include "utils/Logger.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unistd.h>

std::mutex Logger::log_mutex;
std::ofstream Logger::log_file;
std::atomic<LogLevel> Logger::min_level{LogLevel::INFO};
bool Logger::use_color = isatty(STDOUT_FILENO) == 1;
const std::string Logger::RESET_COLOR = "\033[0m";

// open the file safely in append mode
void Logger::init(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(log_mutex);
    log_file.open(filepath, std::ios::app);
    if (!log_file.is_open()) {
        std::cerr << "[FATAL] Could not open log file: " << filepath << "\n";
    }
}

std::string Logger::get_current_time() {
    auto now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&time, &local);  // std::localtime is not thread-safe
    std::stringstream ss;
    ss << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

std::string Logger::level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:   return "DEBUG";
        case LogLevel::INFO:    return "INFO";
        case LogLevel::WARNING: return "WARN";
        case LogLevel::ERROR:   return "ERROR";
    }
    return "UNKNOWN";
}

std::string Logger::level_to_color(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:   return "\033[36m"; // Cyan
        case LogLevel::INFO:    return "\033[32m"; // Green
        case LogLevel::WARNING: return "\033[33m"; // Yellow
        case LogLevel::ERROR:   return "\033[31m"; // Red
    }
    return RESET_COLOR;
}

void Logger::log(LogLevel level, const std::string& message) {
    if (!enabled(level)) return;

    std::string line = "[" + get_current_time() + "] [" + level_to_string(level) + "] " + message;

    std::lock_guard<std::mutex> lock(log_mutex);
    if (use_color) {
        std::cout << level_to_color(level) << line << RESET_COLOR << "\n";
    } else {
        std::cout << line << "\n";
    }

    if (log_file.is_open()) {
        log_file << line << "\n";
        // Flushing every line puts a disk write behind the mutex; only do it
        // for messages we can't afford to lose if the process dies.
        if (level >= LogLevel::WARNING) log_file.flush();
    }
}
