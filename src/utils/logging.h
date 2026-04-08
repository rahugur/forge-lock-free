#pragma once
// Thin wrapper over spdlog for Forge runtime logging.
// Provides macros and a simple init/configure API.

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <mutex>
#include <string>

namespace forge {
namespace logging {

/// Initialize the default Forge logger.
/// Thread-safe and idempotent — safe to call from multiple threads.
inline void init(const std::string& level = "info") {
    static std::once_flag flag;
    std::call_once(flag, [&level]() {
        auto console = spdlog::stdout_color_mt("forge");
        console->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [tid %t] %v");
        spdlog::set_default_logger(console);
        spdlog::set_level(spdlog::level::from_str(level));
    });
}

/// Change the log level at runtime.
inline void set_level(const std::string& level) {
    spdlog::set_level(spdlog::level::from_str(level));
}

}  // namespace logging
}  // namespace forge

// Convenience macros (forward to spdlog's default logger)
#define FORGE_LOG_TRACE(...)    SPDLOG_TRACE(__VA_ARGS__)
#define FORGE_LOG_DEBUG(...)    SPDLOG_DEBUG(__VA_ARGS__)
#define FORGE_LOG_INFO(...)     SPDLOG_INFO(__VA_ARGS__)
#define FORGE_LOG_WARN(...)     SPDLOG_WARN(__VA_ARGS__)
#define FORGE_LOG_ERROR(...)    SPDLOG_ERROR(__VA_ARGS__)
#define FORGE_LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)
