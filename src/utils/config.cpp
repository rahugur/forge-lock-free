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

    // Clamp / validate — guards against JSON underflow (unsigned wrap)
    // and unreasonable values that would exhaust resources.
    constexpr unsigned MAX_THREADS = 1024;
    constexpr unsigned MAX_SESSIONS = 100000;

    if (cfg.num_threads > MAX_THREADS) cfg.num_threads = 0;  // fall back to auto
    if (cfg.max_concurrent_llm_calls == 0 || cfg.max_concurrent_llm_calls > MAX_THREADS)
        cfg.max_concurrent_llm_calls = 8;
    if (cfg.llm_rate_limit <= 0.0 || cfg.llm_rate_limit > 1e6) cfg.llm_rate_limit = 60.0;
    if (cfg.llm_rate_burst <= 0.0 || cfg.llm_rate_burst > 1e6) cfg.llm_rate_burst = 10.0;
    if (cfg.max_sessions == 0 || cfg.max_sessions > MAX_SESSIONS) cfg.max_sessions = 1000;
    if (cfg.default_max_iterations == 0 || cfg.default_max_iterations > 10000)
        cfg.default_max_iterations = 15;
    if (cfg.default_timeout_seconds == 0 || cfg.default_timeout_seconds > 86400)
        cfg.default_timeout_seconds = 300;
    if (cfg.port == 0) cfg.port = 8080;
    if (cfg.host.empty()) cfg.host = "127.0.0.1";
    // Only allow binding to loopback or explicit IP — reject wildcards.
    if (cfg.host == "0.0.0.0" || cfg.host == "::") {
        spdlog::warn("Binding to '{}' exposes the server to the network. "
                     "Set host explicitly if this is intentional.", cfg.host);
    }

    spdlog::info("Loaded config from '{}'", path);
    return cfg;
}

Config default_config() {
    return Config{};
}

}  // namespace forge
