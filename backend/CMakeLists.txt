cmake_minimum_required(VERSION 3.20)
project(molink-core LANGUAGES CXX)

# The Win7 x86 MinGW kit carries GCC 8.1, whose newest standard is gnu++2a
# with an incomplete library. The code only needs C++17 there; pass
# -DMOLINK_CXX_STANDARD=17 for that build.
set(MOLINK_CXX_STANDARD 20 CACHE STRING
  "C++ standard for molink-core (17 for Win7 x86 / GCC 8)")
set(CMAKE_CXX_STANDARD ${MOLINK_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
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
set(MOLINK_UVCPP_PREFIX "" CACHE PATH
  "Unpacked libuvcpp release archive (include/ and lib/, bin/ on Windows)")

if(MOLINK_BUILD_CORE)
  if(MOLINK_UVCPP_PREFIX)
    # CI uses the release archive directly instead of building libuvcpp from
    # source. It ships no CMake package config, so create an imported target.
    if(NOT EXISTS "${MOLINK_UVCPP_PREFIX}/include/uvcpp/uvcpp_config.h")
      message(FATAL_ERROR
        "MOLINK_UVCPP_PREFIX does not look like a libuvcpp release archive: "
        "${MOLINK_UVCPP_PREFIX}")
    endif()
    add_library(uvcpp_prebuilt SHARED IMPORTED)
    set_target_properties(uvcpp_prebuilt PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${MOLINK_UVCPP_PREFIX}/include")
    if(WIN32)
      find_library(_uvp_implib NAMES uvcpp libuvcpp
        PATHS "${MOLINK_UVCPP_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
      find_file(_uvp_dll NAMES uvcpp.dll libuvcpp.dll
        PATHS "${MOLINK_UVCPP_PREFIX}/bin" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
      if(NOT _uvp_implib OR NOT _uvp_dll)
        message(FATAL_ERROR
          "libuvcpp release archive is missing uvcpp.lib or uvcpp.dll")
      endif()
      set_target_properties(uvcpp_prebuilt PROPERTIES
        IMPORTED_IMPLIB "${_uvp_implib}"
        IMPORTED_LOCATION "${_uvp_dll}")
    else()
      find_file(_uvp_so NAMES libuvcpp.so libuvcpp.dylib
        PATHS "${MOLINK_UVCPP_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
      if(NOT _uvp_so)
        message(FATAL_ERROR
          "libuvcpp release archive is missing lib/libuvcpp.so")
      endif()
      set_target_properties(uvcpp_prebuilt PROPERTIES
        IMPORTED_LOCATION "${_uvp_so}")
    endif()
    set(UVP_UVCPP_TARGET uvcpp_prebuilt)
    set(UVP_UVCPP_PREFIX "${MOLINK_UVCPP_PREFIX}")
    set(UVP_UVCPP_ROUTE "prebuilt archive (${MOLINK_UVCPP_PREFIX})")
  else()
  find_package(uvcpp 1.7.0 CONFIG QUIET)

  if(uvcpp_FOUND)
    set(UVP_UVCPP_TARGET uvcpp::uvcpp)
    set(UVP_UVCPP_ROUTE "CMake package (${uvcpp_DIR})")

    # The installed uvcpp package advertises <prefix>/include/libuv even when
    # libuv was not installed there. CMake treats a missing imported include
    # directory as a hard error at generate time, so drop those entries and add
    # the prefix include/lib paths that actually exist.
    get_target_property(_uvp_include_dirs uvcpp::uvcpp INTERFACE_INCLUDE_DIRECTORIES)
    if(_uvp_include_dirs)
      set(_uvp_clean_includes "")
      foreach(_dir ${_uvp_include_dirs})
        if(EXISTS "${_dir}")
          list(APPEND _uvp_clean_includes "${_dir}")
        endif()
      endforeach()
      # uvcpp_DIR is <prefix>/lib/cmake/uvcpp
      get_filename_component(_uvp_prefix "${uvcpp_DIR}/../../.." ABSOLUTE)
      if(EXISTS "${_uvp_prefix}/include")
        list(APPEND _uvp_clean_includes "${_uvp_prefix}/include")
      endif()
      set_target_properties(uvcpp::uvcpp PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_uvp_clean_includes}")
      set(UVP_UVCPP_PREFIX "${_uvp_prefix}")
    endif()
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(UVCPP REQUIRED IMPORTED_TARGET "uvcpp>=1.7.0")
    set(UVP_UVCPP_TARGET PkgConfig::UVCPP)
    set(UVP_UVCPP_ROUTE "pkg-config (${UVCPP_VERSION})")
  endif()
  endif()

  message(STATUS "uvp: libuvcpp via ${UVP_UVCPP_ROUTE}")
endif()

if(MOLINK_BUILD_CORE)
  # ICE/P2P uses the external-socket mode added in Antruly/libjuice. Point
  # MOLINK_LIBJUICE_DIR at a local checkout for offline builds; otherwise CI
  # fetches the pinned fork commit.
  set(MOLINK_LIBJUICE_DIR "" CACHE PATH
    "Patched libjuice source tree (external socket mode)")
  if(MOLINK_LIBJUICE_DIR)
    add_subdirectory("${MOLINK_LIBJUICE_DIR}" "${CMAKE_BINARY_DIR}/libjuice"
                     EXCLUDE_FROM_ALL)
  else()
    include(FetchContent)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    FetchContent_Declare(libjuice
      GIT_REPOSITORY https://github.com/Antruly/libjuice.git
      GIT_TAG ea7fdce765311634103193bfcc5370916d2c0230)
    FetchContent_MakeAvailable(libjuice)
  endif()
  set(MOLINK_LIBJUICE_TARGET LibJuice::LibJuiceStatic)
endif()

set(MOLINK_CORE_SOURCES
  src/main.cpp
  src/api.cpp
  src/chat_hub.cpp
  src/file_transfer.cpp
  src/call_hub.cpp
  src/server_client.cpp
  src/audio/audio.cpp
  src/audio/windows_audio.cpp
  src/audio/windows_voice.cpp
  src/capture/capture.cpp
  src/capture/synthetic_capture.cpp
  src/capture/x11_capture.cpp
  src/capture/windows_capture.cpp
  src/capture/windows_gdi_capture.cpp
  src/capture/windows_secure_capture.cpp
  src/capture/frame_encode.cpp
  src/capture/dirty_rects.cpp
  src/capture/av1_encoder.cpp
  src/clipboard/x11_clipboard.cpp
  src/clipboard/windows_clipboard.cpp
  src/access_requests.cpp
  src/auth_state.cpp
  src/compression.cpp
  src/discovery.cpp
  src/settings.cpp
  src/ui_store.cpp
  src/input/x11_input.cpp
  src/input/windows_input.cpp
  src/input/windows_secure_input.cpp
  src/input/macos_input.cpp
  src/config.cpp
  src/identity.cpp
  src/tls_identity.cpp
  src/lock_state.cpp
  src/session_registry.cpp
  src/service_control.cpp
  src/test_pattern.cpp
  src/transport/ws_transport.cpp
  src/transport/juice_transport.cpp
  src/transport/relay_transport.cpp
  src/transport/switchable_transport.cpp
  src/transport/connect_proxy.cpp
  src/p2p/ice_agent.cpp
  src/p2p/p2p_manager.cpp
  src/util.cpp
  src/session_controller.cpp
  src/protocol/frame_packet.cpp
)

if(APPLE)
  enable_language(OBJCXX)
  list(APPEND MOLINK_CORE_SOURCES
    src/capture/macos_capture.mm
    src/clipboard/macos_clipboard.mm)
endif()

# AV1 rectangle encoding is optional. SVT-AV1 encodes large changed
# rectangles; dav1d decodes them on the receiving core. Without either library
# the build keeps the tile path and the encoder interface reports itself
# unavailable.
#
# The development packages are enough; point MOLINK_AV1_SDK at an unpacked
# tree (usr/include + usr/lib/<triplet>) when they are not installed:
#
#   cmake -S backend -B backend/build -DMOLINK_ENABLE_AV1=ON \
#     -DMOLINK_AV1_SDK=/path/to/av1-sdk
option(MOLINK_ENABLE_AV1
  "Enable AV1 rectangle encoding (needs SVT-AV1, dav1d)" OFF)
set(MOLINK_AV1_SDK "" CACHE PATH
  "Unpacked AV1 dev tree (usr/include, usr/lib) when the packages are absent")
set(MOLINK_SVT_AV1_FOUND 0)
set(MOLINK_DAV1D_FOUND 0)
if(MOLINK_ENABLE_AV1)
  if(MOLINK_AV1_SDK)
    find_path(SVT_AV1_INCLUDE_DIR EbSvtAv1Enc.h
      HINTS "${MOLINK_AV1_SDK}/usr/include" PATH_SUFFIXES svt-av1)
    find_path(DAV1D_INCLUDE_DIR dav1d/dav1d.h
      HINTS "${MOLINK_AV1_SDK}/usr/include")
    find_library(SVT_AV1_LIBRARY NAMES SvtAv1Enc
      HINTS "${MOLINK_AV1_SDK}/usr/lib/x86_64-linux-gnu"
            "${MOLINK_AV1_SDK}/usr/lib")
    find_library(DAV1D_LIBRARY NAMES dav1d
      HINTS "${MOLINK_AV1_SDK}/usr/lib/x86_64-linux-gnu"
            "${MOLINK_AV1_SDK}/usr/lib")
  else()
    find_path(SVT_AV1_INCLUDE_DIR EbSvtAv1Enc.h PATH_SUFFIXES svt-av1)
    find_path(DAV1D_INCLUDE_DIR dav1d/dav1d.h)
    find_library(SVT_AV1_LIBRARY NAMES SvtAv1Enc)
    find_library(DAV1D_LIBRARY NAMES dav1d)
  endif()
  if(SVT_AV1_INCLUDE_DIR AND SVT_AV1_LIBRARY)
    set(MOLINK_SVT_AV1_FOUND 1)
  endif()
  if(DAV1D_INCLUDE_DIR AND DAV1D_LIBRARY)
    set(MOLINK_DAV1D_FOUND 1)
  endif()
  if(NOT MOLINK_SVT_AV1_FOUND)
    message(STATUS "molink: SVT-AV1 not found; AV1 encoding disabled")
  endif()
  if(NOT MOLINK_DAV1D_FOUND)
    message(STATUS "molink: dav1d not found; AV1 decoding disabled")
  endif()
endif()

if(MOLINK_BUILD_CORE)
  add_executable(molink-core ${MOLINK_CORE_SOURCES})
  target_include_directories(molink-core PRIVATE src)

  # Per-device self-signed certificate generation for the encrypted
  # transports. The runtime OpenSSL used by libuvcpp is a separate copy; the
  # certificate is exchanged as PEM, so the two never share objects.
  # OpenSSL generates the per-device certificate. Builds without it (some
  # macOS runner images) keep the encryption settings but report the identity
  # as unavailable instead of failing the whole release.
  find_package(OpenSSL QUIET)
  if(OpenSSL_FOUND)
    target_link_libraries(molink-core PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(molink-core PRIVATE MOLINK_HAVE_OPENSSL=1)
    message(STATUS "molink: device certificate support enabled")
  else()
    target_compile_definitions(molink-core PRIVATE MOLINK_HAVE_OPENSSL=0)
    message(STATUS "molink: OpenSSL not found; encrypted transport disabled")
  endif()

  if(MOLINK_SVT_AV1_FOUND)
    target_compile_definitions(molink-core PRIVATE MOLINK_HAVE_SVT_AV1=1)
    target_include_directories(molink-core PRIVATE ${SVT_AV1_INCLUDE_DIR})
    target_link_libraries(molink-core PRIVATE ${SVT_AV1_LIBRARY})
    message(STATUS "molink: AV1 rectangle encoding enabled (SVT-AV1)")
  endif()
  if(MOLINK_DAV1D_FOUND)
    target_compile_definitions(molink-core PRIVATE MOLINK_HAVE_DAV1D=1)
    target_link_libraries(molink-core PRIVATE ${DAV1D_LIBRARY})
    message(STATUS "molink: AV1 rectangle decoding enabled (dav1d)")
  endif()

  # Linux screen capture. X11/XShm is the first backend; a PipeWire portal
  # backend can be added behind the same capture_source interface.
  set(MOLINK_HAVE_X11 0)
  if(UNIX AND NOT APPLE)
    find_package(X11 QUIET)
    if(X11_FOUND AND TARGET X11::Xext)
      set(MOLINK_HAVE_X11 1)
    endif()
  endif()

  if(WIN32)
    target_link_libraries(molink-core PRIVATE
      d3d11 dxgi dxguid gdi32 user32 ole32 uuid ws2_32 iphlpapi advapi32 shell32)
    if(MINGW)
      # WASAPI format GUIDs (KSDATAFORMAT_SUBTYPE_*) live in ksguid on MinGW;
      # MSVC pulls them in through the SDK's default libraries.
      target_link_libraries(molink-core PRIVATE ksguid)
    endif()
  endif()
  if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
     CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    # GCC 8 keeps std::filesystem in a separate runtime library.
    target_link_libraries(molink-core PRIVATE stdc++fs)
  endif()
  if(APPLE)
    target_link_libraries(molink-core PRIVATE
      "-framework ApplicationServices" "-framework AppKit" "-framework Foundation")
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

  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    target_compile_definitions(molink-core PRIVATE MOLINK_HAVE_ZLIB=1)
    target_link_libraries(molink-core PRIVATE ZLIB::ZLIB)
    message(STATUS "molink: zlib tile compression enabled")
  else()
    target_compile_definitions(molink-core PRIVATE MOLINK_HAVE_ZLIB=0)
    message(STATUS "molink: zlib not found; tile compression disabled")
  endif()

  # Plain `uv` in uvcpp's interface needs a search path on platforms where
  # libuv is not a system library (Windows, macOS).
  if(UVP_UVCPP_PREFIX)
    target_include_directories(molink-core PRIVATE "${UVP_UVCPP_PREFIX}/include")
    target_link_directories(molink-core PRIVATE
      "${UVP_UVCPP_PREFIX}/lib" "${UVP_UVCPP_PREFIX}/bin")
    foreach(_uvinc "${UVP_UVCPP_PREFIX}/include" "${UVP_UVCPP_PREFIX}/include/libuv")
      if(EXISTS "${_uvinc}/uv.h")
        target_include_directories(molink-core PRIVATE "${_uvinc}")
      endif()
    endforeach()
  endif()

  # The P2P socket and timer use libuvcpp's own wrappers, so they run on the
  # same libuv instance as the rest of the core and need no extra libuv link.
  target_link_libraries(molink-core PRIVATE ${UVP_UVCPP_TARGET}
    ${MOLINK_LIBJUICE_TARGET} ${CMAKE_DL_LIBS})

  # libuvcpp's installed libuvcpp.so depends on libllhttp.so in its own directory
  # but carries no RUNPATH of its own. The executable's default RUNPATH is not
  # inherited by indirect dependencies, so keep the linker's legacy RPATH (which
  # is inherited) for the build tree. Release packaging still bundles the shared
  # libraries explicitly.
  if(UNIX AND NOT APPLE)
    # $ORIGIN covers a core packaged beside its libraries; ../lib is the
    # /opt/molink/bin + /opt/molink/lib layout used by the .deb.
    target_link_options(molink-core PRIVATE "LINKER:--disable-new-dtags"
                                          "LINKER:-rpath,$ORIGIN"
                                          "LINKER:-rpath,$ORIGIN/../lib")
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

# Rectangle merging is pure computation: no libuvcpp and no AV1 SDK needed.
add_executable(molink-core-dirty-rects-test
  tests/dirty_rects_test.cpp
  src/capture/dirty_rects.cpp
)
target_include_directories(molink-core-dirty-rects-test PRIVATE src)
set_target_properties(molink-core-dirty-rects-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${uvp_config}" uvp_config_upper)
  set_target_properties(molink-core-dirty-rects-test PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()
add_test(NAME dirty_rects COMMAND molink-core-dirty-rects-test)

# The round-trip test needs both halves of the AV1 SDK.
if(MOLINK_SVT_AV1_FOUND AND MOLINK_DAV1D_FOUND)
  add_executable(molink-core-av1-test
    tests/av1_roundtrip_test.cpp
    src/capture/av1_encoder.cpp
  )
  target_include_directories(molink-core-av1-test PRIVATE
    src ${SVT_AV1_INCLUDE_DIR} ${DAV1D_INCLUDE_DIR})
  target_compile_definitions(molink-core-av1-test PRIVATE MOLINK_HAVE_SVT_AV1=1)
  target_link_libraries(molink-core-av1-test PRIVATE
    ${SVT_AV1_LIBRARY} ${DAV1D_LIBRARY})
  set_target_properties(molink-core-av1-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${uvp_config}" uvp_config_upper)
    set_target_properties(molink-core-av1-test PROPERTIES
      "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
  endforeach()
  add_test(NAME av1_roundtrip COMMAND molink-core-av1-test)
endif()

if (MSVC)
  # MSVC reads a source without a BOM in the system code page. On a Chinese
  # Windows install that is GBK (936), so a UTF-8 source with Chinese comments
  # or string literals is decoded as GBK and fails to compile. /utf-8 tells the
  # compiler both charsets are UTF-8.
  if(MOLINK_BUILD_CORE)
    target_compile_options(molink-core PRIVATE /W4 /permissive- /utf-8)
  endif()
  target_compile_options(molink-core-protocol-test PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(molink-core-dirty-rects-test PRIVATE /W4 /permissive- /utf-8)
  if(TARGET molink-core-av1-test)
    target_compile_options(molink-core-av1-test PRIVATE /W4 /permissive- /utf-8)
  endif()
else()
  if(MOLINK_BUILD_CORE)
    target_compile_options(molink-core PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_compile_options(molink-core-protocol-test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(molink-core-dirty-rects-test PRIVATE -Wall -Wextra -Wpedantic)
  if(TARGET molink-core-av1-test)
    target_compile_options(molink-core-av1-test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()
