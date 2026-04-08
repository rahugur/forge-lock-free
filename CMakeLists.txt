cmake_minimum_required(VERSION 3.20)
project(forge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

option(FORGE_ENABLE_TSAN "Enable Thread Sanitizer" OFF)
if(FORGE_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

option(FORGE_ENABLE_ASAN "Enable Address Sanitizer" OFF)
if(FORGE_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()

include(FetchContent)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3 GIT_SHALLOW TRUE)

FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.13.0 GIT_SHALLOW TRUE)

FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3 GIT_SHALLOW TRUE)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2 GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(nlohmann_json spdlog httplib Catch2)

add_subdirectory(src)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    enable_testing()
    add_subdirectory(tests)
endif()

option(FORGE_BUILD_BENCHMARKS "Build benchmark suite" OFF)
if(FORGE_BUILD_BENCHMARKS)
    add_subdirectory(tests/benchmark)
endif()
