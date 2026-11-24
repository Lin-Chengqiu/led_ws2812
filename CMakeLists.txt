cmake_minimum_required(VERSION 3.13)
project(ws2812 C)

# 核心：只有两个文件，直接加进你的工程也可以
add_library(ws2812 STATIC ws2812.c)
target_include_directories(ws2812 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(ws2812 PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ws2812 PRIVATE -Wall -Wextra)
endif()

# 主机端单元测试：cmake -B build && cmake --build build && ctest --test-dir build
option(WS2812_BUILD_TESTS "Build host unit tests" ON)
if(WS2812_BUILD_TESTS)
    enable_testing()
    add_executable(test_ws2812 tests/test_ws2812.c)
    target_link_libraries(test_ws2812 PRIVATE ws2812)
    add_test(NAME ws2812 COMMAND test_ws2812)
endif()
