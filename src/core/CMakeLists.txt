# AdbRT 核心静态库：命令表、argv 构造、进程封装、解析器、设备会话、配置、日志
add_library(adbrt_core STATIC
  core.cpp
  command_builder.cpp
  command_table.cpp
  parsers.cpp
  adb_runner.cpp
  device.cpp
  settings.cpp
)

target_include_directories(adbrt_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${ADB_GENERATED_DIR}")

target_link_libraries(adbrt_core PUBLIC advapi32 shell32 ole32 user32)
