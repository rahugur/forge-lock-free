# ── Required packages ──────────────────────────────────────────────
find_package(Threads REQUIRED)

# Try to find OpenSSL for HTTPS support (needed for LLM API calls).
find_package(OpenSSL QUIET)
if(OPENSSL_FOUND)
    message(STATUS "OpenSSL found — HTTPS enabled")
    add_compile_definitions(CPPHTTPLIB_OPENSSL_SUPPORT)
else()
    message(WARNING "OpenSSL not found — HTTPS disabled (LLM API calls will fail)")
endif()

# ── forge_core: concurrency primitives ─────────────────────────────
add_library(forge_core
    core/thread_pool.cpp
)
target_include_directories(forge_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forge_core PUBLIC spdlog::spdlog httplib::httplib Threads::Threads)
if(OPENSSL_FOUND)
    target_link_libraries(forge_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# ── forge_utils: config, logging, json helpers ─────────────────────
add_library(forge_utils
    utils/config.cpp
)
target_include_directories(forge_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forge_utils PUBLIC forge_core nlohmann_json::nlohmann_json spdlog::spdlog)

# ── forge_workflow: ReAct and workflow engine ──────────────────────
add_library(forge_workflow
    workflow/react.cpp
    workflow/plan_execute.cpp
    workflow/map_reduce.cpp
)
target_include_directories(forge_workflow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forge_workflow PUBLIC forge_core forge_utils spdlog::spdlog)

# ── forge_session: session manager ─────────────────────────────────
add_library(forge_session
    session/session_manager.cpp
)
target_include_directories(forge_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forge_session PUBLIC forge_workflow forge_core forge_utils spdlog::spdlog)

# ── forge_server: HTTP API server ──────────────────────────────────
add_library(forge_server
    server/http_server.cpp
)
target_include_directories(forge_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forge_server PUBLIC forge_session forge_core forge_utils spdlog::spdlog)

# ── forge executable ───────────────────────────────────────────────
add_executable(forge main.cpp)
target_link_libraries(forge PRIVATE forge_server forge_session forge_workflow forge_core forge_utils)
