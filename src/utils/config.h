#pragma once
// Runtime configuration for the Forge engine.
// Loaded from a JSON file with sensible defaults.

#include <string>
#include <cstdint>

namespace forge {

struct Config {
    // Thread pool
    unsigned num_threads = 0;  // 0 = auto-detect (hardware_concurrency)

    // LLM client
    unsigned max_concurrent_llm_calls = 8;
    double llm_rate_limit = 60.0;     // requests per second
    double llm_rate_burst = 10.0;     // burst size

    // Session limits
    unsigned max_sessions = 1000;
    unsigned default_max_iterations = 15;
    unsigned default_timeout_seconds = 300;

    // Logging
    std::string log_level = "info";

    // Server
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
};

/// Load config from a JSON file. Missing fields keep their defaults.
Config load_config(const std::string& path);

/// Load config from defaults (no file).
Config default_config();

}  // namespace forge
