cmake_minimum_required(VERSION 3.20)
project(moLinkWindows LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
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

add_executable(moLink WIN32 main.cpp)
target_include_directories(moLink PRIVATE "${WEBVIEW2_SDK}/build/native/include")
if(MINGW)
  # MinGW-w64 omits EventToken.h; a minimal shim lives next to this file.
  target_include_directories(moLink PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/compat")
  target_compile_options(moLink PRIVATE -fms-extensions)
endif()
target_link_libraries(moLink PRIVATE user32 gdi32 ole32 oleaut32 shell32)

add_custom_command(TARGET moLink POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${WEBVIEW2_SDK}/build/native/${MOLINK_WEBVIEW2_ARCH}/WebView2Loader.dll"
    "$<TARGET_FILE_DIR:moLink>/WebView2Loader.dll")

install(TARGETS moLink RUNTIME DESTINATION .)
