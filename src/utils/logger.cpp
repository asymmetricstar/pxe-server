#include "utils/logger.hpp"
#include <cstdio>

namespace pxe {

namespace {
const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARNING: return "WARN";
        case LogLevel::ERR: return "ERROR";
    }
    return "?";
}
} // namespace

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

void Logger::setFile(const std::string& path) {
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    file_.open(path, std::ios::app);
    if (!file_.is_open()) {
        std::cerr << "Logger: cannot open log file " << path << std::endl;
    }
}

void Logger::log(LogLevel level, const std::string& message) {
    if (level < level_) return;
    write(level, message);
}

void Logger::write(LogLevel level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &time);
#else
    localtime_r(&time, &tm_buf);
#endif

    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d",
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<int>(ms.count()));

    std::printf("[%s] [%s] %s\n", stamp, levelName(level), msg.c_str());
    std::fflush(stdout);

    if (file_.is_open()) {
        file_ << '[' << stamp << "] [" << levelName(level) << "] " << msg << '\n';
        file_.flush();
    }
}

} // namespace pxe
