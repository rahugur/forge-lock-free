list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

function(forge_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE forge_core forge_utils Catch2::Catch2WithMain)
    catch_discover_tests(${name})
endfunction()

forge_add_test(test_mpsc_queue        core/test_mpsc_queue.cpp)
forge_add_test(test_work_stealing     core/test_work_stealing_deque.cpp)
forge_add_test(test_thread_pool       core/test_thread_pool.cpp)
forge_add_test(test_concurrent_map    core/test_concurrent_map.cpp)
forge_add_test(test_rate_limiter      core/test_rate_limiter.cpp)
forge_add_test(test_async_http_client core/test_async_http_client.cpp)

# Phase 1 tests need forge_workflow (which pulls in forge_core + forge_utils)
function(forge_add_workflow_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE forge_workflow forge_core forge_utils Catch2::Catch2WithMain)
    catch_discover_tests(${name})
endfunction()

forge_add_workflow_test(test_calculator      tools/test_calculator.cpp)
forge_add_workflow_test(test_tool_executor   tools/test_tool_executor.cpp)
forge_add_workflow_test(test_conversation    session/test_conversation.cpp)
forge_add_workflow_test(test_react           workflow/test_react.cpp)

# Stress tests
forge_add_workflow_test(test_stress          stress/test_stress.cpp)

# Phase 2 tests need forge_server (which pulls in forge_session + forge_workflow)
function(forge_add_server_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE forge_server forge_session forge_workflow forge_core forge_utils Catch2::Catch2WithMain)
    catch_discover_tests(${name})
endfunction()

forge_add_test(test_semaphore                core/test_semaphore.cpp)
forge_add_server_test(test_session_manager   session/test_session_manager.cpp)
forge_add_server_test(test_http_server       server/test_http_server.cpp)
forge_add_server_test(test_concurrent_sessions stress/test_concurrent_sessions.cpp)
forge_add_server_test(test_sse              server/test_sse.cpp)

# Phase 3 tests — workflow factory, advanced workflows, utilities
forge_add_workflow_test(test_workflow_factory workflow/test_workflow_factory.cpp)
forge_add_workflow_test(test_plan_execute     workflow/test_plan_execute.cpp)
forge_add_workflow_test(test_map_reduce       workflow/test_map_reduce.cpp)
forge_add_workflow_test(test_task_dag         workflow/test_task_dag.cpp)
forge_add_workflow_test(test_cost_tracker     session/test_cost_tracker.cpp)
forge_add_workflow_test(test_metrics          utils/test_metrics.cpp)
forge_add_workflow_test(test_summarizing_conv session/test_summarizing_conversation.cpp)
