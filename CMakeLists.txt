cmake_minimum_required(VERSION 3.25)
project(realitymap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    # UTF-8 源码与执行字符集（Windows 默认 GBK，统一为 UTF-8）
    add_compile_options(/utf-8 /W4)
    # fopen 等安全 CRT 函数已改用可移植写法（跨 Windows/Linux），关闭 MSVC 的弃用提示
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(CTest)   # 定义 BUILD_TESTING(默认 ON) 并 enable_testing() —— 必须在 add_subdirectory 之前
add_subdirectory(server)
