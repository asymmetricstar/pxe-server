#pragma once
#include <string>
#include <iostream>
#include <fstream>
#include <chrono>
#include <mutex>

namespace pxe {

enum class LogLevel { DEBUG, INFO, WARNING, ERR };

class Logger {
public:
    static Logger& instance();
    void log(LogLevel level, const std::string& message);
    void setLevel(LogLevel level);
    void setFile(const std::string& path);
    void debug(const std::string& msg) { log(LogLevel::DEBUG, msg); }
    void info(const std::string& msg) { log(LogLevel::INFO, msg); }
    void warn(const std::string& msg) { log(LogLevel::WARNING, msg); }
    void error(const std::string& msg) { log(LogLevel::ERR, msg); }
private:
    Logger() = default;
    LogLevel level_ = LogLevel::INFO;
    std::mutex mutex_;
    std::ofstream file_;
    void write(LogLevel level, const std::string& msg);
};

} // namespace pxe
