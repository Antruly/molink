cmake_minimum_required(VERSION 3.20)
project(molink-core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# libuvcpp has to have been built with the WebSocket modules on. If you are
# building it from source, configure it with:
#
#   cmake -S libuvcpp -B build-libuvcpp \
#     -DUVCPP_BUILD_WEB=ON \
#     -DUVCPP_BUILD_WEBAPP=ON \
#     -DCMAKE_INSTALL_PREFIX=/path/to/prefix
#
set(UVP_REQUESTED_FEATURES websocket)

option(MOLINK_BUILD_CORE
  "Build the molink-core executable (requires libuvcpp with WEB and WEBAPP)" ON)

# libuvcpp is found two ways, because it ships two ways and neither covers the
# other:
#
#   - `cmake --install` writes a package config (lib/cmake/uvcpp), so
#     `find_package(uvcpp)` works and yields the namespaced target
#     `uvcpp::uvcpp`.
#   - The prebuilt release archives ship pkg-config and *no* CMake config file,
#     so for those the only way in is `uvcpp.pc`.
#
# Point UVCPP_ROOT or CMAKE_PREFIX_PATH at an unpacked release if it is not
# installed in a standard prefix.
if(MOLINK_BUILD_CORE)
  find_package(uvcpp 1.7.0 CONFIG QUIET)

  if(uvcpp_FOUND)
    set(UVP_UVCPP_TARGET uvcpp::uvcpp)
    set(UVP_UVCPP_ROUTE "CMake package (${uvcpp_DIR})")
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(UVCPP REQUIRED IMPORTED_TARGET "uvcpp>=1.7.0")
    set(UVP_UVCPP_TARGET PkgConfig::UVCPP)
    set(UVP_UVCPP_ROUTE "pkg-config (${UVCPP_VERSION})")
  endif()

  message(STATUS "uvp: libuvcpp via ${UVP_UVCPP_ROUTE}")
endif()

set(MOLINK_CORE_SOURCES
  src/main.cpp
  src/api.cpp
  src/capture/capture.cpp
  src/capture/synthetic_capture.cpp
  src/capture/x11_capture.cpp
  src/config.cpp
  src/session_registry.cpp
  src/test_pattern.cpp
  src/util.cpp
  src/ws_session.cpp
  src/protocol/frame_packet.cpp
)

if(MOLINK_BUILD_CORE)
  add_executable(molink-core ${MOLINK_CORE_SOURCES})
  target_include_directories(molink-core PRIVATE src)

  # Linux screen capture. X11/XShm is the first backend; a PipeWire portal
  # backend can be added behind the same capture_source interface.
  set(MOLINK_HAVE_X11 0)
  if(UNIX AND NOT APPLE)
    find_package(X11 QUIET)
    if(X11_FOUND AND TARGET X11::Xext)
      set(MOLINK_HAVE_X11 1)
    endif()
  endif()

  if(MOLINK_HAVE_X11)
    target_compile_definitions(molink-core PRIVATE MOLINK_HAVE_X11=1)
    target_link_libraries(molink-core PRIVATE X11::X11 X11::Xext)
    message(STATUS "molink: X11 screen capture enabled")
  else()
    target_compile_definitions(molink-core PRIVATE MOLINK_HAVE_X11=0)
    message(STATUS "molink: X11 screen capture unavailable; synthetic frames only")
  endif()

  # The manifest names the executable, and uvp resolves it as build/<name>.
  # Multi-configuration generators (Visual Studio, Xcode) would otherwise append a
  # per-configuration directory and put it in build/Debug/<name>, so the project
  # would build and then not be startable by the tool that generated it.
  set_target_properties(molink-core PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${uvp_config}" uvp_config_upper)
    set_target_properties(molink-core PROPERTIES
      "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
  endforeach()

  target_link_libraries(molink-core PRIVATE ${UVP_UVCPP_TARGET} ${CMAKE_DL_LIBS})

  # libuvcpp's installed libuvcpp.so depends on libllhttp.so in its own directory
  # but carries no RUNPATH of its own. The executable's default RUNPATH is not
  # inherited by indirect dependencies, so keep the linker's legacy RPATH (which
  # is inherited) for the build tree. Release packaging still bundles the shared
  # libraries explicitly.
  if(UNIX AND NOT APPLE)
    target_link_options(molink-core PRIVATE "LINKER:--disable-new-dtags"
                                          "LINKER:-rpath,$ORIGIN")
  elseif(APPLE)
    target_link_options(molink-core PRIVATE "LINKER:-rpath,@loader_path")
  endif()
endif()

# The protocol codec has no libuvcpp dependency, so its tests build and run
# even on a machine without a compatible libuvcpp.
add_executable(molink-core-protocol-test
  tests/frame_packet_test.cpp
  src/protocol/frame_packet.cpp
)
target_include_directories(molink-core-protocol-test PRIVATE src)
set_target_properties(molink-core-protocol-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${uvp_config}" uvp_config_upper)
  set_target_properties(molink-core-protocol-test PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()

enable_testing()
add_test(NAME protocol COMMAND molink-core-protocol-test)

if (MSVC)
  # MSVC reads a source without a BOM in the system code page. On a Chinese
  # Windows install that is GBK (936), so a UTF-8 source with Chinese comments
  # or string literals is decoded as GBK and fails to compile. /utf-8 tells the
  # compiler both charsets are UTF-8.
  if(MOLINK_BUILD_CORE)
    target_compile_options(molink-core PRIVATE /W4 /permissive- /utf-8)
  endif()
  target_compile_options(molink-core-protocol-test PRIVATE /W4 /permissive- /utf-8)
else()
  if(MOLINK_BUILD_CORE)
    target_compile_options(molink-core PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_compile_options(molink-core-protocol-test PRIVATE -Wall -Wextra -Wpedantic)
endif()
