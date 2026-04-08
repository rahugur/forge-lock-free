// Forge — Lock-Free Agent Orchestration Runtime
// Phase 2: Multi-session HTTP server + single-shot CLI.

#include "utils/config.h"
#include "utils/logging.h"
#include "core/thread_pool.h"
#include "core/async_http_client.h"
#include "core/rate_limiter.h"
#include "core/semaphore.h"
#include "llm/llm_client.h"
#include "llm/throttled_llm_client.h"
#include "llm/message.h"
#include "tools/tool_registry.h"
#include "tools/tool_executor.h"
#include "tools/builtin/calculator.h"
#include "tools/builtin/file_read.h"
#include "tools/builtin/shell.h"
#include "tools/builtin/web_search.h"
#include "session/session.h"
#include "session/session_manager.h"
#include "server/http_server.h"
#include "workflow/react.h"
#include "workflow/plan_execute.h"
#include "workflow/map_reduce.h"
#include "workflow/workflow_factory.h"

#include <spdlog/spdlog.h>

#include <csignal>
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
  ║   Lock-Free Agent Orchestration Runtime    v1.0.0     ║
  ╚═══════════════════════════════════════════════════════╝
)" << std::endl;
}

static void print_usage() {
    std::cout << "Usage: forge [OPTIONS]\n"
              << "\n"
              << "Modes:\n"
              << "  -p, --prompt <text>       Run a single prompt (CLI mode)\n"
              << "  -s, --serve               Start HTTP API server\n"
              << "\n"
              << "Options:\n"
              << "  -c, --config <path>       Path to JSON config file\n"
              << "  -w, --workflow <name>     Workflow pattern (default: react)\n"
              << "  -t, --tools <path>        Tool selection JSON file\n"
              << "  -m, --model <name>        LLM model override\n"
              << "  -k, --api-key <key>       API key (or set OPENAI_API_KEY env)\n"
              << "      --api-base <url>      API base URL (default: https://api.openai.com)\n"
              << "      --max-iterations <n>  Max ReAct steps (default: 15)\n"
              << "  -v, --verbose             Enable debug logging\n"
              << "  -h, --help                Show this help\n";
}

// Global server pointer for signal handler.
static forge::HttpServer* g_server = nullptr;

static void signal_handler(int) {
    if (g_server) g_server->stop();
}

int main(int argc, char* argv[]) {
    // Parse CLI args.
    std::string config_path;
    std::string workflow = "react";
    std::string prompt;
    std::string tools_path;
    std::string model;
    std::string api_key;
    std::string api_base;
    int max_iterations = -1;
    bool verbose = false;
    bool serve = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << arg << "\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") { print_usage(); return 0; }
        else if (arg == "-c" || arg == "--config") config_path = next();
        else if (arg == "-w" || arg == "--workflow") workflow = next();
        else if (arg == "-p" || arg == "--prompt") prompt = next();
        else if (arg == "-s" || arg == "--serve") serve = true;
        else if (arg == "-t" || arg == "--tools") tools_path = next();
        else if (arg == "-m" || arg == "--model") model = next();
        else if (arg == "-k" || arg == "--api-key") api_key = next();
        else if (arg == "--api-base") api_base = next();
        else if (arg == "--max-iterations") max_iterations = std::stoi(next());
        else if (arg == "-v" || arg == "--verbose") verbose = true;
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage();
            return 1;
        }
    }

    // Load config.
    forge::Config cfg = config_path.empty()
                            ? forge::default_config()
                            : forge::load_config(config_path);

    // Apply CLI overrides.
    if (verbose) cfg.log_level = "debug";
    if (max_iterations > 0) cfg.default_max_iterations = static_cast<unsigned>(max_iterations);

    // Initialize logging.
    forge::logging::init(cfg.log_level);
    print_banner();

    if (!serve && prompt.empty()) {
        spdlog::error("No mode selected. Use -p/--prompt or -s/--serve.");
        print_usage();
        return 1;
    }

    // Build shared runtime components.
    forge::ThreadPool pool(cfg.num_threads);
    spdlog::info("Thread pool: {} workers", pool.num_threads());

    forge::AsyncHttpClient http(pool, {
        /*connect_timeout_sec=*/10,
        /*read_timeout_sec=*/120,
        /*block_private_ips=*/false
    });

    forge::LLMClientConfig llm_cfg;
    if (!api_base.empty()) llm_cfg.api_base = api_base;
    if (!model.empty()) llm_cfg.model = model;
    if (!api_key.empty()) llm_cfg.api_key = api_key;
    forge::LLMClient raw_llm(http, llm_cfg);

    // Backpressure: rate limiter + concurrency semaphore.
    forge::RateLimiter rate(cfg.llm_rate_limit, cfg.llm_rate_burst);
    forge::Semaphore sem(static_cast<int>(cfg.max_concurrent_llm_calls));
    forge::ThrottledLLMClient llm(raw_llm, sem, rate);

    spdlog::info("LLM: {} @ {} (max_concurrent={}, rate={}/s)",
                 raw_llm.config().model, raw_llm.config().api_base,
                 cfg.max_concurrent_llm_calls, cfg.llm_rate_limit);

    // Register builtin tools.
    forge::ToolRegistry registry;
    forge::builtin::register_calculator(registry);
    forge::builtin::register_file_read(registry);
    forge::builtin::register_shell(registry);
    forge::builtin::register_web_search(registry);

    spdlog::info("Tools registered: {}", registry.size());

    forge::ToolExecutor executor(pool, registry);

    // Build workflow factory.
    forge::WorkflowFactory factory;
    factory.register_workflow("react", [](forge::ILLMClient& l, forge::ToolExecutor& ex,
                                          const forge::ToolRegistry& reg, forge::ThreadPool&) {
        return std::make_unique<forge::ReActWorkflow>(l, ex, reg);
    });
    factory.register_workflow("plan-execute", [](forge::ILLMClient& l, forge::ToolExecutor& ex,
                                                 const forge::ToolRegistry& reg, forge::ThreadPool& p) {
        return std::make_unique<forge::PlanExecuteWorkflow>(l, ex, reg, p);
    });
    factory.register_workflow("map-reduce", [](forge::ILLMClient& l, forge::ToolExecutor& ex,
                                               const forge::ToolRegistry& reg, forge::ThreadPool& p) {
        return std::make_unique<forge::MapReduceWorkflow>(l, ex, reg, p);
    });

    if (!factory.has(workflow)) {
        spdlog::error("Unknown workflow '{}'. Available: {}", workflow,
                      [&]() {
                          std::string s;
                          for (auto& n : factory.names()) {
                              if (!s.empty()) s += ", ";
                              s += n;
                          }
                          return s;
                      }());
        return 1;
    }

    if (serve) {
        // ── Server mode ──────────────────────────────────────────
        forge::SessionManager manager(pool, llm, executor, registry,
                                      factory, cfg.max_sessions);

        forge::HttpServer server(manager, cfg.host, cfg.port);
        g_server = &server;

        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        server.start();  // blocks until stop()

        g_server = nullptr;
        spdlog::info("Server shut down.");
    } else {
        // ── CLI mode (single prompt) ─────────────────────────────
        forge::SessionConfig session_cfg;
        session_cfg.max_iterations = cfg.default_max_iterations;
        session_cfg.timeout = std::chrono::seconds(cfg.default_timeout_seconds);

        forge::Session session(/*id=*/1, session_cfg);

        forge::Message sys_msg;
        sys_msg.role = "system";
        sys_msg.content = "You are a helpful assistant with access to tools. "
                          "Use tools when needed to answer questions accurately. "
                          "When you have the final answer, respond directly without calling tools.";
        session.history.push(std::move(sys_msg));

        forge::Message user_msg;
        user_msg.role = "user";
        user_msg.content = prompt;
        session.history.push(std::move(user_msg));

        auto wf = factory.create(workflow, llm, executor, registry, pool);
        spdlog::info("Running '{}' workflow with prompt: \"{}\"",
                     wf->name(), prompt.substr(0, 80));

        auto result = wf->run(session);

        if (result.success) {
            std::cout << "\n" << result.answer << std::endl;
        } else {
            std::cerr << "\nError: " << result.error << std::endl;
            return 1;
        }

        spdlog::info("Done. Steps: {}, Tokens: {}+{}",
                     result.steps_taken,
                     result.total_prompt_tokens,
                     result.total_completion_tokens);
    }

    return 0;
}
