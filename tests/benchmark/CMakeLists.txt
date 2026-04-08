add_executable(bench_sessions bench_sessions.cpp)
target_include_directories(bench_sessions PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(bench_sessions PRIVATE
    forge_server forge_session forge_workflow forge_core forge_utils)
