cmake_minimum_required(VERSION 3.20)
project(moLinkSecureHelper LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(molink-helper main.cpp)
target_link_libraries(molink-helper PRIVATE advapi32 wtsapi32 user32 gdi32)
if(MINGW)
  target_link_options(molink-helper PRIVATE -municode)
  # WTSSessionInfoEx / WTSINFOEXW / WTS_SESSIONSTATE_LOCK are guarded by
  # _WIN32_WINNT >= 0x0601 in the mingw-w64 headers, and the Win7 x86 kit
  # leaves the macro at its Vista-era default. The package targets Windows 7,
  # so ask for exactly that API level.
  target_compile_definitions(molink-helper PRIVATE
    _WIN32_WINNT=0x0601 WINVER=0x0601)
endif()
install(TARGETS molink-helper RUNTIME DESTINATION .)
