#include "utils/config.h"
#include "utils/json.h"

#include <spdlog/spdlog.h>

namespace forge {

Config load_config(const std::string& path) {
    Config cfg;

    Json j;
    try {
        j = json_util::parse_file(path);
    } catch (const std::exception& e) {
        spdlog::warn("Failed to load config from '{}': {}. Using defaults.", path, e.what());
        return cfg;
    }

    // Helper: read a field if it exists, otherwise keep default
    auto read = [&](const char* key, auto& field) {
        if (j.contains(key)) {
            field = j[key].get<std::remove_reference_t<decltype(field)>>();
        }
    };

    read("num_threads",              cfg.num_threads);
    read("max_concurrent_llm_calls", cfg.max_concurrent_llm_calls);
    read("llm_rate_limit",           cfg.llm_rate_limit);
    read("llm_rate_burst",           cfg.llm_rate_burst);
    read("max_sessions",             cfg.max_sessions);
    read("default_max_iterations",   cfg.default_max_iterations);
    read("default_timeout_seconds",  cfg.default_timeout_seconds);
    read("log_level",                cfg.log_level);
    read("host",                     cfg.host);
    read("port",                     cfg.port);

    spdlog::info("Loaded config from '{}'", path);
    return cfg;
}

Config default_config() {
    return Config{};
}

}  // namespace forge
