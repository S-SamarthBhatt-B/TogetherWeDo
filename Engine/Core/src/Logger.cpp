#include "TogetherEngine/Core/Logger.h"

#include <fstream>
#include <iostream>
#include <string>

namespace together::core {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::set_file(const std::filesystem::path& path) {
    std::scoped_lock lock(mutex_);
    path_ = path;
}

void Logger::write(const LogLevel level, const std::string_view message) {
    const char* label = "INFO";
    if (level == LogLevel::Warning) label = "WARN";
    if (level == LogLevel::Error) label = "ERROR";
    const std::string line = std::string("[Together Engine][") + label + "] " + std::string(message);
    std::scoped_lock lock(mutex_);
    std::clog << line << '\n';
    if (!path_.empty()) {
        std::ofstream file(path_, std::ios::app);
        if (file) file << line << '\n';
    }
}

} // namespace together::core
