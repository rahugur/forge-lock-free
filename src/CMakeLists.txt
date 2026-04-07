# ── Required packages ──────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── forge_core: concurrency primitives ─────────────────────────────
add_library(forge_core
    core/thread_pool.cpp
)
target_include_directories(forge_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forge_core PUBLIC spdlog::spdlog Threads::Threads)

# ── forge_utils: config, logging, json helpers ─────────────────────
add_library(forge_utils
    utils/config.cpp
)
target_include_directories(forge_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forge_utils PUBLIC forge_core nlohmann_json::nlohmann_json spdlog::spdlog)

# ── forge executable ───────────────────────────────────────────────
add_executable(forge main.cpp)
target_link_libraries(forge PRIVATE forge_core forge_utils)
