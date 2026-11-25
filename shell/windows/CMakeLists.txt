cmake_minimum_required(VERSION 3.20)
project(moLinkWindows LANGUAGES C CXX RC)

set(MOLINK_CXX_STANDARD 20 CACHE STRING
  "C++ standard for the Windows shell (17 for Win7 x86 / GCC 8)")
set(CMAKE_CXX_STANDARD ${MOLINK_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(WEBVIEW2_SDK "" CACHE PATH "Root of the Microsoft.Web.WebView2 NuGet package")
if(NOT EXISTS "${WEBVIEW2_SDK}/build/native/include/WebView2.h")
  message(FATAL_ERROR
    "Set -DWEBVIEW2_SDK=<nuget package root> (it must contain build/native/include/WebView2.h)")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    set(MOLINK_WEBVIEW2_ARCH arm64)
  else()
    set(MOLINK_WEBVIEW2_ARCH x64)
  endif()
else()
  set(MOLINK_WEBVIEW2_ARCH x86)
endif()

set(MOLINK_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../assets/icons/icon.ico")
if(NOT EXISTS "${MOLINK_ICON_PATH}")
  message(FATAL_ERROR "Application icon not found: ${MOLINK_ICON_PATH}")
endif()
# Keep forward slashes in the generated .rc: rc.exe treats backslashes in
# quoted paths as escape sequences (e.g. \a, \m), which breaks the path.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/app.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
add_executable(moLink WIN32 main.cpp surface_window.cpp "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
target_include_directories(moLink PRIVATE "${WEBVIEW2_SDK}/build/native/include")
if(MINGW)
  # MinGW-w64 omits EventToken.h; a minimal shim lives next to this file.
  target_include_directories(moLink PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/compat")
  target_compile_options(moLink PRIVATE -fms-extensions)
endif()
# winhttp: the loopback /api/device probe that lets this shell adopt a core
# already started by the moLinkSession service/helper instead of spawning a
# second one that cannot bind the port.
target_link_libraries(moLink PRIVATE
  user32 gdi32 ole32 oleaut32 shell32 urlmon winhttp)

add_custom_command(TARGET moLink POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${WEBVIEW2_SDK}/build/native/${MOLINK_WEBVIEW2_ARCH}/WebView2Loader.dll"
    "$<TARGET_FILE_DIR:moLink>/WebView2Loader.dll")

install(TARGETS moLink RUNTIME DESTINATION .)

# The native Direct2D client (no WebView2). CI builds this too so the
# session/preset sources, which the WebView2 shell does not compile, cannot
# change without a Windows compiler checking them.
add_executable(moLink-native WIN32
  ui_kit_demo.cpp
  ui_kit.cpp
  ws_client.cpp
  session_client.cpp
  file_panel_logic.cpp
  preset_message.cpp
  adaptive_quality.cpp
  display_selector.cpp
  trackpad_mouse.cpp
  clipboard_sync.cpp
  clipboard_files.cpp
  input_state.cpp
  native_diag.cpp
  error_policy.cpp
  reconnect_policy.cpp
  ui_strings.cpp
  watermark_fade.cpp
  puff.c
  ui_kit_demo.rc
)
target_include_directories(moLink-native PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  # ui_kit_demo.rc names the icon as shell/assets/icons/icon.ico.
  "${CMAKE_CURRENT_SOURCE_DIR}/../.."
)
# NOMINMAX: windows.h defines min/max as macros, which turns every
# std::min/std::max in the shared UI headers into a syntax error under MSVC
# (C2589). The native shell had only ever been built with MinGW, so the first
# MSVC build stopped here.
target_compile_definitions(moLink-native PRIVATE _WIN32_WINNT=0x0601 NOMINMAX)
if(MSVC)
  # preset_message.h carries the zh-CN labels as UTF-8 source text.
  target_compile_options(moLink-native PRIVATE /utf-8)
elseif(MINGW)
  target_link_options(moLink-native PRIVATE -municode)
endif()
target_link_libraries(moLink-native PRIVATE
  d2d1 dwrite ole32 oleaut32 user32 gdi32 advapi32
  windowscodecs comdlg32 ws2_32 winhttp shell32 uuid
)

install(TARGETS moLink-native RUNTIME DESTINATION .)

# Host-only shell (issue #97 / roadmap H3+H4): a fixed Direct2D 1.0 window
# with status, network and security cards. It has no WebView2 dependency and
# no remote-desktop/file/chat/terminal UI. The state, settings and service
# modules are platform-neutral so they can be compiled and tested without
# Win32; only host_only/main.cpp owns the window and the HTTP poller.
add_executable(moLink-host WIN32
  host_only/main.cpp
  host_only/host_state.cpp
  host_only/host_settings.cpp
  host_only/service_ctl.cpp
)
target_include_directories(moLink-host PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/host_only")
target_compile_definitions(moLink-host PRIVATE
  _WIN32_WINNT=0x0601 NOMINMAX UNICODE _UNICODE)
# The host-only UI deliberately stays at C++17 so the Win7 x86 toolchain can
# build it as well as the MSVC x64 CI job.
set_target_properties(moLink-host PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
if(MSVC)
  # The UI strings are UTF-8 in the source; /utf-8 also keeps the narrow
  # service/state error strings (converted to wide at the boundary) correct.
  target_compile_options(moLink-host PRIVATE /utf-8)
elseif(MINGW)
  # __uuidof(IDWriteFactory) needs the MS extension; -municode selects
  # wWinMain as the entry point.
  target_compile_options(moLink-host PRIVATE -fms-extensions)
  target_link_options(moLink-host PRIVATE -municode)
endif()
target_link_libraries(moLink-host PRIVATE
  d2d1 dwrite ole32 user32 gdi32 shell32 winhttp advapi32 uuid
)
install(TARGETS moLink-host RUNTIME DESTINATION .)
