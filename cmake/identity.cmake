# ---------------------------------------------------------------------------
# 身份与版本唯一真相源：packaging/identity.json
#   → 导出 ADB_ID_* / ADB_VERSION / ADB_ARCH，并生成 <build>/generated/version.h
#   改这里 = 改版本号；exe 的 FileVersion 与发布包名都从它派生。
# ---------------------------------------------------------------------------
set(ADB_IDENTITY_FILE "${CMAKE_SOURCE_DIR}/packaging/identity.json")
if(NOT EXISTS "${ADB_IDENTITY_FILE}")
  message(FATAL_ERROR "缺少身份文件：${ADB_IDENTITY_FILE}")
endif()

file(READ "${ADB_IDENTITY_FILE}" ADB_ID_JSON)

function(adb_json_get key out)
  if(ADB_ID_JSON MATCHES "\"${key}\"[ \t]*:[ \t]*\"([^\"]*)\"")
    set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  else()
    set(${out} "" PARENT_SCOPE)
  endif()
endfunction()

adb_json_get("version"      ADB_ID_VERSION)
adb_json_get("productName"  ADB_ID_PRODUCT)
adb_json_get("exeName"      ADB_ID_EXE)
adb_json_get("description"  ADB_ID_DESCRIPTION)
adb_json_get("companyName"  ADB_ID_COMPANY)

if(NOT ADB_ID_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "identity.json 的 version 必须是 a.b.c.d 四段式，当前值：'${ADB_ID_VERSION}'")
endif()

set(ADB_VERSION "${ADB_ID_VERSION}" CACHE STRING "版本号（派生自 identity.json）" FORCE)
message(STATUS "AdbRT ${ADB_ID_PRODUCT} ${ADB_VERSION}")

# FileVersion 资源头（由 version_info.rc 用 include 引进来）
set(ADB_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${ADB_GENERATED_DIR}")

string(REPLACE "." "," ADB_ID_VERSION_COMMA "${ADB_ID_VERSION}")

configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/version.h.in"
  "${ADB_GENERATED_DIR}/version.h" @ONLY)
