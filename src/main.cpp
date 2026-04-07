// Forge — Lock-Free Agent Orchestration Runtime
// Phase 0: Foundation entry point.

#include "utils/config.h"
#include "utils/logging.h"
#include "core/thread_pool.h"

#include <spdlog/spdlog.h>

#include <iostream>
#include <string>

static void print_banner() {
    std::cout << R"(
  ╔═══════════════════════════════════════════════════════╗
  ║    _____ ___  ____   ____ _____                      ║
  ║   |  ___/ _ \|  _ \ / ___| ____|                     ║
  ║   | |_ | | | | |_) | |  _|  _|                       ║
  ║   |  _|| |_| |  _ <| |_| | |___                      ║
  ║   |_|   \___/|_| \_\\____|_____|                     ║
  ║                                                       ║
  ║   Lock-Free Agent Orchestration Runtime    v0.1.0     ║
  ╚═══════════════════════════════════════════════════════╝
)" << std::endl;
}

static void print_config(const forge::Config& cfg) {
    spdlog::info("Configuration:");
    spdlog::info("  num_threads:              {}", cfg.num_threads == 0
                     ? std::to_string(std::thread::hardware_concurrency()) + " (auto)"
                     : std::to_string(cfg.num_threads));
    spdlog::info("  max_concurrent_llm_calls: {}", cfg.max_concurrent_llm_calls);
    spdlog::info("  llm_rate_limit:           {:.1f} req/s (burst: {:.0f})",
                 cfg.llm_rate_limit, cfg.llm_rate_burst);
    spdlog::info("  max_sessions:             {}", cfg.max_sessions);
    spdlog::info("  default_max_iterations:   {}", cfg.default_max_iterations);
    spdlog::info("  default_timeout:          {}s", cfg.default_timeout_seconds);
    spdlog::info("  log_level:                {}", cfg.log_level);
    spdlog::info("  server:                   {}:{}", cfg.host, cfg.port);
}

int main(int argc, char* argv[]) {
    print_banner();

    // Parse CLI args
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: forge [OPTIONS]\n"
                      << "  -c, --config <path>   Path to JSON config file\n"
                      << "  -h, --help            Show this help\n";
            return 0;
        }
    }

    // Load config
    forge::Config cfg = config_path.empty()
                            ? forge::default_config()
                            : forge::load_config(config_path);

    // Initialize logging
    forge::logging::init(cfg.log_level);

    print_config(cfg);

    // Quick self-test: verify thread pool starts and shuts down
    {
        spdlog::info("Thread pool self-test...");
        forge::ThreadPool pool(cfg.num_threads);
        auto fut = pool.submit([]() -> int { return 42; });
        int result = fut.get();
        spdlog::info("  submit(→42).get() = {} ✓", result);
        spdlog::info("Thread pool OK ({} workers)", pool.num_threads());
    }

    spdlog::info("Phase 0 foundation ready. Workflow engine coming in Phase 1.");
    return 0;
}
