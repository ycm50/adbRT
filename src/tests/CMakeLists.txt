# 无窗口核心单元测试（不依赖设备、不依赖桌面会话）
add_executable(AdbRT.CoreTests core_tests.cpp)

target_include_directories(AdbRT.CoreTests PRIVATE
  "${CMAKE_SOURCE_DIR}/src/core"
  "${ADB_GENERATED_DIR}")

target_link_libraries(AdbRT.CoreTests PRIVATE adbrt_core)

add_test(NAME core_tests COMMAND AdbRT.CoreTests)
