cmake_minimum_required(VERSION 3.14)

project(VortexServer VERSION 1.1 DESCRIPTION "Event-driven static HTTP/1.1 server" LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Vortex uses epoll and sendfile, so it only builds on Linux (WSL works).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Optional sanitizers, e.g. -DVORTEX_SANITIZE=address,undefined or -DVORTEX_SANITIZE=thread
set(VORTEX_SANITIZE "" CACHE STRING "Comma-separated list of sanitizers to enable")
if(VORTEX_SANITIZE)
    add_compile_options(-fsanitize=${VORTEX_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${VORTEX_SANITIZE})
endif()

find_package(Threads REQUIRED)

# Everything except main() goes in a library so the tests can link against it.
add_library(vortex_core STATIC
    src/concurrency/ThreadPool.cpp
    src/http/Request.cpp
    src/http/Response.cpp
    src/http/StaticFiles.cpp
    src/network/Connection.cpp
    src/network/EpollServer.cpp
    src/utils/Logger.cpp
)
target_include_directories(vortex_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_options(vortex_core PUBLIC -Wall -Wextra -Wpedantic -Werror=return-type)
target_link_libraries(vortex_core PUBLIC Threads::Threads)

add_executable(vortex_server src/main.cpp)
target_link_libraries(vortex_server PRIVATE vortex_core)

# Tests: run with `ctest --test-dir build --output-on-failure`
enable_testing()

add_executable(test_parser tests/test_parser.cpp)
target_link_libraries(test_parser PRIVATE vortex_core)
add_test(NAME parser COMMAND test_parser)

add_executable(test_static_files tests/test_static_files.cpp)
target_link_libraries(test_static_files PRIVATE vortex_core)
add_test(NAME static_files COMMAND test_static_files)

add_executable(test_pool tests/test_threadpool.cpp)
target_link_libraries(test_pool PRIVATE vortex_core)
add_test(NAME threadpool COMMAND test_pool all)

add_executable(test_server tests/test_server.cpp)
target_link_libraries(test_server PRIVATE vortex_core)
add_test(NAME server COMMAND test_server)
