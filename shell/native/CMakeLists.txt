cmake_minimum_required(VERSION 3.20)
project(moLinkShell LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_SKIP_BUILD_RPATH TRUE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET webkit2gtk-4.1)

add_executable(moLink main.cpp surface_window.cpp)
target_link_libraries(moLink PRIVATE PkgConfig::GTK3 PkgConfig::WEBKIT)
target_compile_options(moLink PRIVATE -Wall -Wextra -Wpedantic)

# Keep the build tree relocatable: the installed layout puts the binary next to
# a lib directory, and the core is found relative to the executable at runtime.
set_target_properties(moLink PROPERTIES
  BUILD_RPATH ""
  INSTALL_RPATH "$ORIGIN/../lib"
)

install(TARGETS moLink RUNTIME DESTINATION bin)
