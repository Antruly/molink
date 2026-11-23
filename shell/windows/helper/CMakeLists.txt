cmake_minimum_required(VERSION 3.20)
project(moLinkSecureHelper LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Restart/backoff decisions are platform-neutral and tested on every host.
add_library(molink-supervisor-policy STATIC supervisor_policy.cpp)
target_include_directories(molink-supervisor-policy
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if(WIN32)
  add_executable(molink-helper main.cpp core_supervisor.cpp)
  target_link_libraries(molink-helper PRIVATE
    molink-supervisor-policy
    advapi32 wtsapi32 user32 gdi32 userenv)
  # The helper writes app.exit lines with the core's own formatter so the
  # diagnostic log keeps one convention, not a second hand-rolled one.
  add_library(molink-diag STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../backend/src/diag_log.cpp")
  target_include_directories(molink-diag
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../../backend/src")
  target_link_libraries(molink-helper PRIVATE molink-diag)
  if(MINGW)
    target_link_options(molink-helper PRIVATE -municode)
  endif()
  install(TARGETS molink-helper RUNTIME DESTINATION .)
endif()

option(MOLINK_HELPER_TESTS "Build the helper's unit tests" ON)
if(MOLINK_HELPER_TESTS)
  enable_testing()
  add_executable(molink-helper-supervisor-test
    tests/supervisor_policy_test.cpp)
  target_link_libraries(molink-helper-supervisor-test
    PRIVATE molink-supervisor-policy)
  add_test(NAME supervisor_policy COMMAND molink-helper-supervisor-test)
endif()
