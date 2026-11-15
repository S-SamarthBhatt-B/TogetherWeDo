#pragma once

#include <filesystem>
#include <mutex>
#include <string_view>

namespace together::core {

enum class LogLevel { Info, Warning, Error };

class Logger final {
public:
    static Logger& instance();
    void set_file(const std::filesystem::path& path);
    void write(LogLevel level, std::string_view message);

private:
    Logger() = default;
    std::mutex mutex_;
    std::filesystem::path path_;
};

} // namespace together::core
