# 无窗口核心单元测试（不依赖设备、不依赖桌面会话）
add_executable(AdbRT.CoreTests core_tests.cpp fixture_loader.cpp)

target_include_directories(AdbRT.CoreTests PRIVATE
  "${CMAKE_SOURCE_DIR}/src/core"
  "${ADB_GENERATED_DIR}")

target_link_libraries(AdbRT.CoreTests PRIVATE adbrt_core)

add_test(NAME core_tests COMMAND AdbRT.CoreTests)
# adb 桩：端到端测试用（不依赖真机）
# MinGW 下 wmain 需要 -municode，否则链接器找的是窄字符 WinMain
add_executable(fake-adb fake_adb.cpp)
if(MINGW)
  target_link_options(fake-adb PRIVATE -municode)
endif()
# 标签解析器的测试样本：拷到可执行文件旁边供 fixture_loader 读取
foreach(fixture fixture.manifest.bin)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${fixture}"
                 "${CMAKE_CURRENT_BINARY_DIR}/${fixture}" COPYONLY)
endforeach()