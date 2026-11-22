cmake_minimum_required(VERSION 3.20)
project(moLinkShell LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_SKIP_BUILD_RPATH TRUE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
# XGetInputFocus/XQueryTree live in libX11; GTK links it transitively but the
# linker will not resolve symbols through gdk-3's DT_NEEDED by default.
# Optional so a Wayland-only GTK build still compiles; main.cpp guards the
# calls with GDK_WINDOWING_X11.
pkg_check_modules(X11 x11 IMPORTED_TARGET)
# The grab itself uses XInput2 (XIGrabDevice), because GTK3 ignores core
# XGrabKeyboard events. Optional: without it the shell reports the grab as
# unsupported instead of locking a keyboard it cannot feed to WebKit.
pkg_check_modules(XI xi IMPORTED_TARGET)

add_executable(moLink main.cpp surface_window.cpp)
target_link_libraries(moLink PRIVATE PkgConfig::GTK3 PkgConfig::WEBKIT)
if(X11_FOUND)
  target_link_libraries(moLink PRIVATE PkgConfig::X11)
endif()
if(XI_FOUND)
  target_link_libraries(moLink PRIVATE PkgConfig::XI)
  target_compile_definitions(moLink PRIVATE MOLINK_HAVE_XI2=1)
endif()
target_compile_options(moLink PRIVATE -Wall -Wextra -Wpedantic)

# Keep the build tree relocatable: the installed layout puts the binary next to
# a lib directory, and the core is found relative to the executable at runtime.
set_target_properties(moLink PROPERTIES
  BUILD_RPATH ""
  INSTALL_RPATH "$ORIGIN/../lib"
)

install(TARGETS moLink RUNTIME DESTINATION bin)
