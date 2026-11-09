# AdbRT.exe —— Win32 GUI（-mwindows，无控制台）
set(ADB_GUI_SOURCES
  main.cpp
  window.cpp
  param_panel.cpp
  task.cpp
  selftest.cpp
  stubs.cpp
  ui_util.cpp
  install_cmd.cpp
  dialogs.cpp
  builtin.cpp
  windows2.cpp
)

# 图标与版本资源：app.rc 里的图标路径在构建期注入绝对路径
#（windres 相对路径的基准目录不可靠，绝对路径最稳）
set(ADB_ASSET_DIR "${CMAKE_SOURCE_DIR}/packaging/assets")
set(ADB_ICON_ICO "${ADB_ASSET_DIR}/adbrt.ico")
set(ADB_ICON_PNG "${ADB_ASSET_DIR}/adbrt-256.png")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/app.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)

add_executable(AdbRT WIN32 ${ADB_GUI_SOURCES}
  "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
  settings_dialog.rc)

target_include_directories(AdbRT PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${ADB_GENERATED_DIR}")

target_link_libraries(AdbRT PRIVATE
  adbrt_core
  comctl32 comdlg32 ole32 shell32 shlwapi uuid)

target_compile_definitions(AdbRT PRIVATE _CRT_SECURE_NO_WARNINGS)

# MinGW：wWinMain 需要 -municode
if(MINGW)
  target_link_options(AdbRT PRIVATE -municode)
endif()