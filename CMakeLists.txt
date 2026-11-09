cmake_minimum_required(VERSION 3.25)
# ---------------------------------------------------------------------------
# adbRT —— Android 平台工具（adb / fastboot）的 Windows GUI 外壳
#   版本号只有一个真相源：packaging/identity.json（见 cmake/identity.cmake）
#   工具链：MSYS2 UCRT64 (MinGW-w64)，C++20，零第三方依赖
# ---------------------------------------------------------------------------
project(AdbRT LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

include(cmake/identity.cmake)

add_compile_definitions(
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 WINVER=0x0A00
  ADBRT_VERSION="${ADB_ID_VERSION}")

add_compile_options(-Wall -Wextra -Wno-unknown-pragmas -Wno-unused-parameter -fno-rtti)
set(CMAKE_RC_FLAGS "--codepage=65001")

if(DEFINED ENV{SOURCE_DATE_EPOCH})
  message(STATUS "可复现构建：SOURCE_DATE_EPOCH=$ENV{SOURCE_DATE_EPOCH}")
  add_compile_options(
    "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=/src"
    "-ffile-prefix-map=${CMAKE_BINARY_DIR}=/build")
endif()

option(ADB_WERROR       "把编译警告当错误" OFF)
option(ADB_BUILD_GUI    "构建 GUI 主程序 AdbRT.exe" ON)
option(ADB_BUILD_TESTS  "构建无窗口核心单元测试并注册进 CTest" ON)

add_subdirectory(src/core)

if(ADB_BUILD_GUI)
  add_subdirectory(src/gui)
endif()

enable_testing()

if(ADB_BUILD_TESTS)
  add_subdirectory(src/tests)
endif()