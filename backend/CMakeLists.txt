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

option(MOLINK_OPTION_WEBTRANSPORT
  "Build the WebTransport session transport (needs libuvcpp with WebTransport)" ON)

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

# The UDP relay transport calls libuv directly. Linux links the system libuv;
# a Windows release archive that statically links libuv does not expose it, so
# that one transport is omitted instead of failing the whole build.
set(MOLINK_HAVE_UDP_RELAY 1)

# WebRTC (Phase 4): an H.264 video track and a data channel per session, with
# signaling over the existing session socket. Off by default: it needs
# libdatachannel, which is not part of any of the release kits yet.
option(MOLINK_ENABLE_WEBRTC
  "Build the WebRTC video track and data channel (needs libdatachannel)" OFF)

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
      find_library(_uvp_uv NAMES uv libuv
        PATHS "${MOLINK_UVCPP_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
      if(_uvp_uv)
        set_target_properties(uvcpp_prebuilt PROPERTIES
          INTERFACE_LINK_LIBRARIES "${_uvp_uv}")
      else()
        # The 1.8.6 MSVC archive statically links libuv and ships no uv.lib.
        # Keep the core linkable; the relay ladder falls back to QUIC/TCP/H2/WS.
        set(MOLINK_HAVE_UDP_RELAY 0)
        message(STATUS
          "molink: libuvcpp archive has no uv.lib; UDP relay transport omitted")
      endif()
    else()
      find_file(_uvp_so NAMES libuvcpp.so libuvcpp.dylib
        PATHS "${MOLINK_UVCPP_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
      if(NOT _uvp_so)
        message(FATAL_ERROR
          "libuvcpp release archive is missing lib/libuvcpp.so")
      endif()
      set_target_properties(uvcpp_prebuilt PROPERTIES
        IMPORTED_LOCATION "${_uvp_so}")
      # The UDP relay transport calls libuv directly. Linux links the system
      # libuv; if it is absent, omit that transport instead of failing to link.
      find_library(_uvp_uv NAMES uv libuv)
      if(_uvp_uv)
        set_target_properties(uvcpp_prebuilt PROPERTIES
          INTERFACE_LINK_LIBRARIES "${_uvp_uv}")
      else()
        set(MOLINK_HAVE_UDP_RELAY 0)
        message(STATUS
          "molink: system libuv not found; UDP relay transport omitted")
      endif()
    endif()
    set(UVP_UVCPP_TARGET uvcpp_prebuilt)
    set(UVP_UVCPP_PREFIX "${MOLINK_UVCPP_PREFIX}")
    set(UVP_UVCPP_LIBDIR "$<TARGET_FILE_DIR:uvcpp_prebuilt>")
    set(UVP_UVCPP_ROUTE "prebuilt archive (${MOLINK_UVCPP_PREFIX})")
  else()
  find_package(uvcpp 1.7.0 CONFIG QUIET)

  if(uvcpp_FOUND)
    set(UVP_UVCPP_TARGET uvcpp::uvcpp)
    set(UVP_UVCPP_LIBDIR "$<TARGET_FILE_DIR:uvcpp::uvcpp>")
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
    set(UVP_UVCPP_PREFIX "${UVCPP_PREFIX}")
    # A release archive's uvcpp.pc may not list libuv in Requires, but the
    # UDP relay transport calls uv_* directly, so link it explicitly.
    pkg_check_modules(UVLIB QUIET IMPORTED_TARGET libuv)
    if(UVLIB_FOUND)
      set(UVP_LIBUV_TARGET PkgConfig::UVLIB)
    else()
      find_library(_uvp_uv NAMES uv libuv)
      if(_uvp_uv)
        set(UVP_LIBUV_TARGET "${_uvp_uv}")
      endif()
    endif()
    # PkgConfig::UVCPP is an interface target, so $<TARGET_FILE_DIR:...> is
    # invalid for it; the -L directory is what the tests' RPATH needs.
    if(UVCPP_LIBDIR)
      set(UVP_UVCPP_LIBDIR "${UVCPP_LIBDIR}")
    elseif(UVCPP_LIBRARY_DIRS)
      list(GET UVCPP_LIBRARY_DIRS 0 UVP_UVCPP_LIBDIR)
    else()
      set(UVP_UVCPP_LIBDIR "${UVCPP_PREFIX}/lib")
    endif()
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
  src/capture/vpx_stream_encoder.cpp
  src/capture/av1_frame.cpp
  src/capture/jpeg_encode.cpp
  src/surface/frame_surface.cpp
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
  src/transport/channel_scheduler.cpp
  src/transport/relay_stream_transport.cpp
  src/transport/quic_relay_transport.cpp
  src/transport/udp_relay_transport.cpp
  src/transport/h3_relay_transport.cpp
  src/transport/tls_pin.cpp
  src/transport/relay_ladder.cpp
  src/transport/switchable_transport.cpp
  src/transport/connect_proxy.cpp
  src/transport/direct_proxy.cpp
  src/transport/stream_transport.cpp
  src/transport/quic_listener.cpp
  src/transport/h3_ws_listener.cpp
  src/transport/h3_ws_transport.cpp
  src/transport/web_ws_transport.cpp
  src/transport/h2_ws_transport.cpp
  src/transport/h2_relay_transport.cpp
  src/transport/tcp_listener.cpp
  src/transport/peer_transports.cpp
  src/p2p/ice_agent.cpp
  src/p2p/p2p_manager.cpp
  src/util.cpp
  src/session_controller.cpp
  src/protocol/frame_packet.cpp
)

# WebTransport (Phase 5) is compiled only when the libuvcpp in use provides it:
# a 1.9 build has the session type and draft-13 stream framing, 1.8 has neither.
set(MOLINK_HAVE_WEBTRANSPORT 0)
if(MOLINK_OPTION_WEBTRANSPORT AND UVP_UVCPP_PREFIX)
  set(_molink_wt_header
      "${UVP_UVCPP_PREFIX}/include/http3/uvcpp_h3_wt_session.h")
  set(_molink_ws_header
      "${UVP_UVCPP_PREFIX}/include/http3/uvcpp_h3_ws_connection.h")
  set(_molink_uvcpp_cfg
      "${UVP_UVCPP_PREFIX}/include/uvcpp/uvcpp_config.h")
  if(EXISTS "${_molink_wt_header}" AND EXISTS "${_molink_ws_header}" AND
     EXISTS "${_molink_uvcpp_cfg}")
    # The header alone is not enough: without HTTP/3 the class inside is
    # compiled out and using it would fail to link. Require the feature bit.
    file(STRINGS "${_molink_uvcpp_cfg}" _molink_uvcpp_http3
         REGEX "^#[ ]*define[ ]+UVCPP_HTTP3_ENABLE[ ]+1")
    if(_molink_uvcpp_http3)
      set(MOLINK_HAVE_WEBTRANSPORT 1)
      list(APPEND MOLINK_CORE_SOURCES
        src/transport/wt_transport.cpp
        src/transport/wt_framing.cpp)
    endif()
  endif()
endif()

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

# VP9/VP8 stream encoding is optional: libvpx provides the encoder cells. The
# page decodes both through WebCodecs. Point MOLINK_VPX_SDK at an unpacked
# libvpx dev tree when the headers are not installed:
#
#   cmake -S backend -B backend/build -DMOLINK_ENABLE_VPX=ON \
#     -DMOLINK_VPX_SDK=/path/to/libvpx-dev-root
option(MOLINK_ENABLE_VPX
  "Enable VP9/VP8 stream encoding (needs libvpx)" OFF)
set(MOLINK_VPX_SDK "" CACHE PATH
  "Unpacked libvpx dev tree (usr/include, usr/lib) when the package is absent")
set(MOLINK_HAVE_VPX 0)
if(MOLINK_ENABLE_VPX)
  if(MOLINK_VPX_SDK)
    find_path(VPX_INCLUDE_DIR vpx/vpx_encoder.h
      HINTS "${MOLINK_VPX_SDK}/usr/include")
    find_library(VPX_LIBRARY NAMES vpx
      HINTS "${MOLINK_VPX_SDK}/usr/lib/x86_64-linux-gnu"
            "${MOLINK_VPX_SDK}/usr/lib")
  else()
    find_path(VPX_INCLUDE_DIR vpx/vpx_encoder.h)
    find_library(VPX_LIBRARY NAMES vpx)
  endif()
  if(VPX_INCLUDE_DIR AND VPX_LIBRARY)
    set(MOLINK_HAVE_VPX 1)
    message(STATUS "molink: VP9/VP8 stream encoding enabled (libvpx ${VPX_LIBRARY})")
  else()
    message(STATUS "molink: libvpx not found; VP9/VP8 streams disabled")
  endif()
endif()

# JPEG for full-screen stream frames handed to the page: the browser decodes a
# JPEG with its native image decoder, which is far cheaper than inflating zlib
# and much smaller than raw pixels. libjpeg-turbo provides JCS_EXT_BGRX, which
# takes our BGRA rows without a conversion.
option(MOLINK_ENABLE_JPEG "Encode stream frames as JPEG (needs libjpeg-turbo)" OFF)
set(MOLINK_JPEG_SDK "" CACHE PATH
  "Unpacked libjpeg-turbo dev tree (usr/include, usr/lib) when absent")
set(MOLINK_HAVE_JPEG 0)
if(MOLINK_ENABLE_JPEG)
  if(MOLINK_JPEG_SDK)
    find_path(JPEG_INCLUDE_DIR jpeglib.h HINTS "${MOLINK_JPEG_SDK}/usr/include")
    find_library(JPEG_LIBRARY NAMES jpeg
      HINTS "${MOLINK_JPEG_SDK}/usr/lib/x86_64-linux-gnu"
            "${MOLINK_JPEG_SDK}/usr/lib")
  else()
    find_path(JPEG_INCLUDE_DIR jpeglib.h)
    find_library(JPEG_LIBRARY NAMES jpeg)
  endif()
  if(JPEG_INCLUDE_DIR AND JPEG_LIBRARY)
    set(MOLINK_HAVE_JPEG 1)
  else()
    message(STATUS "molink: libjpeg-turbo not found; stream frames stay raw")
  endif()
endif()

# NVIDIA NVENC AV1 rectangles. The header is vendored under third_party, so
# the feature is available on any build with the NVIDIA driver at runtime; the
# encoder is loaded with LoadLibrary/dlopen and no import library is needed.
option(MOLINK_ENABLE_NVENC "Enable NVIDIA NVENC AV1 rectangles" ON)
set(MOLINK_NVENC_SDK "" CACHE PATH
  "Optional ffnvcodec include tree (contains ffnvcodec/nvEncodeAPI.h)")
set(MOLINK_HAVE_NVENC 0)
if(MOLINK_ENABLE_NVENC)
  find_path(NVENC_INCLUDE_DIR ffnvcodec/nvEncodeAPI.h
    HINTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
          "${MOLINK_NVENC_SDK}/include")
  if(NVENC_INCLUDE_DIR AND EXISTS
     "${NVENC_INCLUDE_DIR}/ffnvcodec/nvEncodeAPI.h")
    set(MOLINK_HAVE_NVENC 1)
    list(APPEND MOLINK_CORE_SOURCES src/capture/nvenc_av1_encoder.cpp)
  endif()
endif()

if(MOLINK_ENABLE_WEBRTC)
  find_package(LibDataChannel CONFIG QUIET)
  if(NOT LibDataChannel_FOUND)
    message(FATAL_ERROR
      "MOLINK_ENABLE_WEBRTC=ON but libdatachannel was not found; point "
      "CMAKE_PREFIX_PATH at its install tree")
  endif()
  # The H.264 video track needs libdatachannel's media half
  # (`rtc::H264RtpPacketizer` lives behind RTC_ENABLE_MEDIA). A media-less
  # build fails deep inside rtc_session.cpp with a wall of "is not a member of
  # rtc" errors, so say what is actually wrong here instead. vcpkg's port turns
  # media off unless its `srtp` feature is enabled (INVERTED_FEATURES srtp
  # NO_MEDIA), i.e. `libdatachannel[srtp]`.
  get_target_property(_molink_rtc_defs LibDataChannel::LibDataChannel
                      INTERFACE_COMPILE_DEFINITIONS)
  if(NOT _molink_rtc_defs OR
     NOT ("RTC_ENABLE_MEDIA=1" IN_LIST _molink_rtc_defs OR
          "RTC_ENABLE_MEDIA" IN_LIST _molink_rtc_defs))
    message(FATAL_ERROR
      "MOLINK_ENABLE_WEBRTC=ON but this libdatachannel was built without "
      "media support (no H.264 RTP packetizer). Rebuild it with media on; "
      "with vcpkg that means the `srtp` feature: libdatachannel[srtp].")
  endif()
  list(APPEND MOLINK_CORE_SOURCES src/webrtc/rtc_session.cpp)
  set(MOLINK_HAVE_WEBRTC 1)
  message(STATUS "molink: WebRTC video track + data channel enabled")
else()
  set(MOLINK_HAVE_WEBRTC 0)
endif()

if(MOLINK_BUILD_CORE)
  if(NOT MOLINK_HAVE_UDP_RELAY)
    list(REMOVE_ITEM MOLINK_CORE_SOURCES src/transport/udp_relay_transport.cpp)
  endif()
  add_executable(molink-core ${MOLINK_CORE_SOURCES})
  target_include_directories(molink-core PRIVATE src)

  # Per-device self-signed certificate generation for the encrypted
  # transports. The runtime OpenSSL used by libuvcpp is a separate copy; the
  # certificate is exchanged as PEM, so the two never share objects.
  # OpenSSL generates the per-device certificate. Builds without it (some
  # macOS runner images) keep the encryption settings but report the identity
  # as unavailable instead of failing the whole release.
  # HTTP/3 WebSocket sessions (RFC 9220) need the h3 WebSocket header that
  # libuvcpp releases started shipping in 1.8.4. Without it the listener still
  # builds but start() reports HTTP/3 as unavailable.
  set(MOLINK_HAVE_H3_WS 0)
  if(UVP_UVCPP_PREFIX AND EXISTS
     "${UVP_UVCPP_PREFIX}/include/http3/uvcpp_h3_ws_connection.h")
    set(MOLINK_HAVE_H3_WS 1)
  endif()
  target_compile_definitions(molink-core PRIVATE
    MOLINK_HAVE_H3_WS=${MOLINK_HAVE_H3_WS}
    MOLINK_HAVE_WEBTRANSPORT=${MOLINK_HAVE_WEBTRANSPORT})

  # WebRTC video track + data channel (Phase 4). The library brings its own
  # libjuice, so ICE behaviour matches the native P2P path.
  if(MOLINK_HAVE_WEBRTC)
    target_compile_definitions(molink-core PRIVATE MOLINK_HAVE_WEBRTC=1)
    target_link_libraries(molink-core PRIVATE LibDataChannel::LibDataChannel)
  else()
    target_compile_definitions(molink-core PRIVATE MOLINK_HAVE_WEBRTC=0)
  endif()

  # HTTP/2 WebSocket (RFC 8441) needs both the h2 WS connection and the
  # webapp's transport-agnostic extended CONNECT session (libuvcpp 1.8.6).
  set(MOLINK_HAVE_H2_WS 0)
  if(UVP_UVCPP_PREFIX AND EXISTS
     "${UVP_UVCPP_PREFIX}/include/http2/uvcpp_h2_ws_connection.h"
     AND EXISTS "${UVP_UVCPP_PREFIX}/include/webapp/uvcpp_web_ws.h")
    set(MOLINK_HAVE_H2_WS 1)
  endif()
  target_compile_definitions(molink-core PRIVATE
    MOLINK_HAVE_H2_WS=${MOLINK_HAVE_H2_WS}
    MOLINK_HAVE_UDP_RELAY=${MOLINK_HAVE_UDP_RELAY})

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
    target_include_directories(molink-core PRIVATE ${DAV1D_INCLUDE_DIR})
    target_link_libraries(molink-core PRIVATE ${DAV1D_LIBRARY})
    message(STATUS "molink: AV1 rectangle decoding enabled (dav1d)")
  endif()
  if(MOLINK_HAVE_VPX)
    target_compile_definitions(molink-core PRIVATE MOLINK_HAVE_VPX=1)
    target_include_directories(molink-core PRIVATE ${VPX_INCLUDE_DIR})
    target_link_libraries(molink-core PRIVATE ${VPX_LIBRARY})
  endif()
  if(MOLINK_HAVE_JPEG)
    target_compile_definitions(molink-core PRIVATE MOLINK_HAVE_JPEG=1)
    target_include_directories(molink-core PRIVATE ${JPEG_INCLUDE_DIR})
    target_link_libraries(molink-core PRIVATE ${JPEG_LIBRARY})
    message(STATUS "molink: JPEG stream frames enabled (libjpeg-turbo)")
  endif()
  if(MOLINK_HAVE_NVENC)
    target_compile_definitions(molink-core PRIVATE MOLINK_HAVE_NVENC=1)
    target_include_directories(molink-core PRIVATE ${NVENC_INCLUDE_DIR})
    if(WIN32)
      target_link_libraries(molink-core PRIVATE d3d11 dxgi dxguid)
    else()
      target_link_libraries(molink-core PRIVATE ${CMAKE_DL_LIBS})
    endif()
    message(STATUS "molink: NVIDIA NVENC AV1 encoder enabled")
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

  find_package(ZLIB MODULE QUIET)
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
  # same libuv instance as the rest of the core. UVP_LIBUV_TARGET is only set
  # for the pkg-config route, whose .pc may omit libuv from its Requires even
  # though the UDP relay transport calls uv_* directly.
  target_link_libraries(molink-core PRIVATE ${UVP_UVCPP_TARGET}
    ${UVP_LIBUV_TARGET} ${MOLINK_LIBJUICE_TARGET} ${CMAKE_DL_LIBS})

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

add_executable(molink-core-replay-policy-test
  tests/replay_policy_test.cpp
)
target_include_directories(molink-core-replay-policy-test PRIVATE src)
set_target_properties(molink-core-replay-policy-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME replay_policy COMMAND molink-core-replay-policy-test)

# The frame-budget adaptation is pure arithmetic: no encoder required.
add_executable(molink-core-frame-budget-test
  tests/frame_budget_test.cpp
)
target_include_directories(molink-core-frame-budget-test PRIVATE src)
set_target_properties(molink-core-frame-budget-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME frame_budget COMMAND molink-core-frame-budget-test)

# Box averaging for large tile strips: pure pixel arithmetic.
add_executable(molink-core-tile-scale-test
  tests/tile_scale_test.cpp
)
target_include_directories(molink-core-tile-scale-test PRIVATE src)
set_target_properties(molink-core-tile-scale-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME tile_scale COMMAND molink-core-tile-scale-test)

# The tile <-> AV1 stream hand-over is pure state machine.
add_executable(molink-core-stream-policy-test
  tests/stream_policy_test.cpp
)
target_include_directories(molink-core-stream-policy-test PRIVATE src)
set_target_properties(molink-core-stream-policy-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME stream_policy COMMAND molink-core-stream-policy-test)

# The rate-driven quality controller: preset in, viewer frame rate out.
add_executable(molink-core-rate-quality-test
  tests/rate_quality_test.cpp
)
target_include_directories(molink-core-rate-quality-test PRIVATE src)
set_target_properties(molink-core-rate-quality-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME rate_quality COMMAND molink-core-rate-quality-test)

# The libvpx stream encoder: keyframe, then cheaper deltas. Skips without a
# libvpx build.
add_executable(molink-core-vpx-stream-test
  tests/vpx_stream_test.cpp
  src/capture/vpx_stream_encoder.cpp
)
target_include_directories(molink-core-vpx-stream-test PRIVATE src)
if(MOLINK_HAVE_VPX)
  target_compile_definitions(molink-core-vpx-stream-test PRIVATE
    MOLINK_HAVE_VPX=1)
  target_include_directories(molink-core-vpx-stream-test PRIVATE
    ${VPX_INCLUDE_DIR})
  target_link_libraries(molink-core-vpx-stream-test PRIVATE ${VPX_LIBRARY})
endif()
set_target_properties(molink-core-vpx-stream-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME vpx_stream COMMAND molink-core-vpx-stream-test)

# WebTransport loopback: a real session over loopback QUIC, both halves of the
# transport (reliable stream + fragmented datagrams). Skips without HTTP/3.
if(MOLINK_HAVE_WEBTRANSPORT AND OpenSSL_FOUND)
  add_executable(molink-core-wt-loopback-test
    tests/wt_loopback_test.cpp
    src/transport/wt_transport.cpp
    src/transport/wt_framing.cpp
    src/transport/stream_transport.cpp
    src/protocol/frame_packet.cpp
    src/tls_identity.cpp
  )
  target_include_directories(molink-core-wt-loopback-test PRIVATE src)
  target_compile_definitions(molink-core-wt-loopback-test PRIVATE
    MOLINK_HAVE_WEBTRANSPORT=1 MOLINK_HAVE_OPENSSL=1)
  if(UVP_UVCPP_PREFIX)
    target_include_directories(molink-core-wt-loopback-test PRIVATE
      "${UVP_UVCPP_PREFIX}/include")
    target_link_directories(molink-core-wt-loopback-test PRIVATE
      "${UVP_UVCPP_PREFIX}/lib")
  endif()
  target_link_libraries(molink-core-wt-loopback-test PRIVATE
    ${UVP_UVCPP_TARGET} OpenSSL::SSL OpenSSL::Crypto)
  set_target_properties(molink-core-wt-loopback-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    BUILD_RPATH "${UVP_UVCPP_LIBDIR}")
  foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${uvp_config}" uvp_config_upper)
    set_target_properties(molink-core-wt-loopback-test PROPERTIES
      "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
  endforeach()
  if(UNIX AND NOT APPLE)
    target_link_options(molink-core-wt-loopback-test PRIVATE
      "LINKER:--disable-new-dtags")
  endif()
  add_test(NAME wt_loopback COMMAND molink-core-wt-loopback-test)
  set_tests_properties(wt_loopback PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Datagram framing for WebTransport: pure byte handling, no libuvcpp needed.
add_executable(molink-core-wt-framing-test
  tests/wt_framing_test.cpp
  src/transport/wt_framing.cpp
)
target_include_directories(molink-core-wt-framing-test PRIVATE src)
set_target_properties(molink-core-wt-framing-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME wt_framing COMMAND molink-core-wt-framing-test)

# The unified encoder face is policy only: the ladder and the backend list.
add_executable(molink-core-video-encoder-test
  tests/video_encoder_test.cpp
  src/capture/av1_encoder.cpp
  src/capture/vpx_stream_encoder.cpp
)
target_include_directories(molink-core-video-encoder-test PRIVATE src)
set_target_properties(molink-core-video-encoder-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME video_encoder COMMAND molink-core-video-encoder-test)

# The GPU-to-CPU hand-over is pure policy over two encoders, so it builds and
# runs without a GPU (or even an encoder library) present.
add_executable(molink-core-av1-stream-fallback-test
  tests/av1_stream_fallback_test.cpp
)
target_include_directories(molink-core-av1-stream-fallback-test PRIVATE src)
set_target_properties(molink-core-av1-stream-fallback-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME av1_stream_fallback COMMAND molink-core-av1-stream-fallback-test)

# The host-side frame clock is pure scheduling, so it needs no transport.
add_executable(molink-core-video-push-test
  tests/video_push_test.cpp
)
target_include_directories(molink-core-video-push-test PRIVATE src)
set_target_properties(molink-core-video-push-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME video_push COMMAND molink-core-video-push-test)

# The logical channel API is backward-compatible with existing transports.
add_executable(molink-core-session-channel-test
  tests/session_transport_channel_test.cpp
)
target_include_directories(molink-core-session-channel-test PRIVATE src)
set_target_properties(molink-core-session-channel-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME session_channel COMMAND molink-core-session-channel-test)

# Channel queues are transport-independent and enforce Phase 1 backpressure.
add_executable(molink-core-channel-scheduler-test
  tests/channel_scheduler_test.cpp
  src/transport/channel_scheduler.cpp
)
target_include_directories(molink-core-channel-scheduler-test PRIVATE src)
set_target_properties(molink-core-channel-scheduler-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME channel_scheduler COMMAND molink-core-channel-scheduler-test)

# The relay/ICE split is pure routing: no libuvcpp and no sockets.
add_executable(molink-core-switchable-test
  tests/switchable_transport_test.cpp
  src/transport/switchable_transport.cpp
  src/protocol/frame_packet.cpp
)
target_include_directories(molink-core-switchable-test PRIVATE src)
set_target_properties(molink-core-switchable-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${uvp_config}" uvp_config_upper)
  set_target_properties(molink-core-switchable-test PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()
add_test(NAME switchable COMMAND molink-core-switchable-test)

# This test compiles the production file-transfer implementation, which needs
# libuvcpp's WebApp JSON header. Keep it out of dependency-free test builds;
# the core build has already resolved UVP_UVCPP_TARGET and provides the header.
if(MOLINK_BUILD_CORE AND UVP_UVCPP_TARGET)
  add_executable(molink-core-file-transfer-backpressure-test
    tests/file_transfer_backpressure_test.cpp
    src/file_transfer.cpp
    src/util.cpp
  )
  target_include_directories(molink-core-file-transfer-backpressure-test PRIVATE
    src ${UVP_UVCPP_PREFIX}/include)
  # The test needs libuvcpp's JSON header, not its symbols: linking the
  # library would drag in libuv (and fail on macOS runners without it).
  if(WIN32)
    # util.cpp enumerates the local addresses with GetAdaptersAddresses
    # (iphlpapi) and formats them with inet_ntop (ws2_32).
    target_link_libraries(molink-core-file-transfer-backpressure-test PRIVATE
      ws2_32 iphlpapi)
  endif()
  set_target_properties(molink-core-file-transfer-backpressure-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  add_test(NAME file_transfer_backpressure
    COMMAND molink-core-file-transfer-backpressure-test)
endif()

# NVENC smoke test: encodes one rectangle and writes the AV1 OBU. Exit 77 on
# machines without an AV1-capable NVIDIA GPU (CTest marks it skipped).
if(MOLINK_HAVE_NVENC)
  add_executable(molink-core-nvenc-test
    tests/nvenc_test.cpp
    src/capture/nvenc_av1_encoder.cpp
  )
  target_include_directories(molink-core-nvenc-test PRIVATE
    src ${NVENC_INCLUDE_DIR})
  if(WIN32)
    target_link_libraries(molink-core-nvenc-test PRIVATE d3d11 dxgi dxguid)
  else()
    target_link_libraries(molink-core-nvenc-test PRIVATE ${CMAKE_DL_LIBS})
  endif()
  set_target_properties(molink-core-nvenc-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${uvp_config}" uvp_config_upper)
    set_target_properties(molink-core-nvenc-test PROPERTIES
      "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
  endforeach()
  add_test(NAME nvenc COMMAND molink-core-nvenc-test)
  set_tests_properties(nvenc PROPERTIES SKIP_RETURN_CODE 77)

  # The low-delay stream encoders: a keyframe, deltas, and a forced resync for
  # each codec the GPU reports. Exit 77 on machines without NVENC.
  add_executable(molink-core-nvenc-stream-test
    tests/nvenc_stream_test.cpp
    src/capture/nvenc_av1_encoder.cpp
    src/capture/av1_encoder.cpp
    src/capture/vpx_stream_encoder.cpp
  )
  target_include_directories(molink-core-nvenc-stream-test PRIVATE
    src ${NVENC_INCLUDE_DIR})
  target_compile_definitions(molink-core-nvenc-stream-test PRIVATE
    MOLINK_HAVE_NVENC=1)
  if(WIN32)
    target_link_libraries(molink-core-nvenc-stream-test PRIVATE d3d11 dxgi dxguid)
  else()
    target_link_libraries(molink-core-nvenc-stream-test PRIVATE ${CMAKE_DL_LIBS})
  endif()
  set_target_properties(molink-core-nvenc-stream-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${uvp_config}" uvp_config_upper)
    set_target_properties(molink-core-nvenc-stream-test PROPERTIES
      "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
  endforeach()
  add_test(NAME nvenc_stream COMMAND molink-core-nvenc-stream-test)
  set_tests_properties(nvenc_stream PROPERTIES SKIP_RETURN_CODE 77)
endif()

# WebRTC loopback: the host session against a peer that speaks the browser's
# side of the handshake, all in one process over loopback ICE.
if(MOLINK_ENABLE_WEBRTC)
  add_executable(molink-core-webrtc-loopback-test
    tests/webrtc_loopback_test.cpp
    src/webrtc/rtc_session.cpp
  )
  target_include_directories(molink-core-webrtc-loopback-test PRIVATE src)
  target_compile_definitions(molink-core-webrtc-loopback-test PRIVATE
    MOLINK_HAVE_WEBRTC=1)
  target_link_libraries(molink-core-webrtc-loopback-test PRIVATE
    LibDataChannel::LibDataChannel)
  set_target_properties(molink-core-webrtc-loopback-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  add_test(NAME webrtc_loopback COMMAND molink-core-webrtc-loopback-test)
endif()

# The round-trip test needs both halves of the AV1 SDK.
if(MOLINK_SVT_AV1_FOUND AND MOLINK_DAV1D_FOUND)
  add_executable(molink-core-av1-test
    tests/av1_roundtrip_test.cpp
    src/capture/av1_encoder.cpp
    src/capture/vpx_stream_encoder.cpp
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

  add_executable(molink-core-av1-frame-test
    tests/av1_frame_test.cpp
    src/capture/av1_frame.cpp
    src/capture/av1_encoder.cpp
    src/capture/vpx_stream_encoder.cpp
    src/protocol/frame_packet.cpp
  )
  target_include_directories(molink-core-av1-frame-test PRIVATE
    src ${SVT_AV1_INCLUDE_DIR} ${DAV1D_INCLUDE_DIR})
  target_compile_definitions(molink-core-av1-frame-test PRIVATE
    MOLINK_HAVE_SVT_AV1=1 MOLINK_HAVE_DAV1D=1)
  target_link_libraries(molink-core-av1-frame-test PRIVATE
    ${SVT_AV1_LIBRARY} ${DAV1D_LIBRARY})
  set_target_properties(molink-core-av1-frame-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${uvp_config}" uvp_config_upper)
    set_target_properties(molink-core-av1-frame-test PROPERTIES
      "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
  endforeach()
  add_test(NAME av1_frame COMMAND molink-core-av1-frame-test)

  add_executable(molink-core-av1-stream-test
    tests/av1_stream_test.cpp
    src/capture/av1_frame.cpp
    src/capture/av1_encoder.cpp
    src/capture/vpx_stream_encoder.cpp
    src/protocol/frame_packet.cpp
  )
  target_include_directories(molink-core-av1-stream-test PRIVATE
    src ${SVT_AV1_INCLUDE_DIR} ${DAV1D_INCLUDE_DIR})
  target_compile_definitions(molink-core-av1-stream-test PRIVATE
    MOLINK_HAVE_SVT_AV1=1 MOLINK_HAVE_DAV1D=1)
  target_link_libraries(molink-core-av1-stream-test PRIVATE
    ${SVT_AV1_LIBRARY} ${DAV1D_LIBRARY})
  set_target_properties(molink-core-av1-stream-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${uvp_config}" uvp_config_upper)
    set_target_properties(molink-core-av1-stream-test PROPERTIES
      "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
  endforeach()
  add_test(NAME av1_stream COMMAND molink-core-av1-stream-test)

  # Capture-side split: large changes become AV1 patches, small ones stay
  # tiles. This exercises frame_encode.cpp with the real encoder.
  find_package(ZLIB MODULE QUIET)
  add_executable(molink-core-frame-encode-av1-test
    tests/frame_encode_av1_test.cpp
    src/capture/frame_encode.cpp
    src/capture/dirty_rects.cpp
    src/capture/av1_frame.cpp
    src/capture/av1_encoder.cpp
    src/capture/vpx_stream_encoder.cpp
    src/protocol/frame_packet.cpp
    src/compression.cpp
  )
  target_include_directories(molink-core-frame-encode-av1-test PRIVATE
    src ${SVT_AV1_INCLUDE_DIR} ${DAV1D_INCLUDE_DIR})
  target_compile_definitions(molink-core-frame-encode-av1-test PRIVATE
    MOLINK_HAVE_SVT_AV1=1 MOLINK_HAVE_DAV1D=1)
  target_link_libraries(molink-core-frame-encode-av1-test PRIVATE
    ${SVT_AV1_LIBRARY} ${DAV1D_LIBRARY})
  if(ZLIB_FOUND)
    target_compile_definitions(molink-core-frame-encode-av1-test PRIVATE
      MOLINK_HAVE_ZLIB=1)
    target_link_libraries(molink-core-frame-encode-av1-test PRIVATE ZLIB::ZLIB)
  endif()
  set_target_properties(molink-core-frame-encode-av1-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${uvp_config}" uvp_config_upper)
    set_target_properties(molink-core-frame-encode-av1-test PROPERTIES
      "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
  endforeach()
  add_test(NAME frame_encode_av1 COMMAND molink-core-frame-encode-av1-test)
endif()

# QUIC transport loopback: one QUIC bidirectional stream carries MOLK packets
# between a server and a client on 127.0.0.1. It needs libuvcpp built with QUIC
# (ngtcp2 + the OpenSSL 3.2 QUIC API) and OpenSSL for the loopback certificate.
# When libuvcpp reports QUIC disabled the binary still builds, prints SKIP and
# exits 77, which CTest records as skipped.
if(MOLINK_BUILD_CORE AND OpenSSL_FOUND)
  add_executable(molink-core-quic-loopback-test
    tests/quic_loopback_test.cpp
    src/transport/stream_transport.cpp
    src/transport/quic_listener.cpp
    src/protocol/frame_packet.cpp
    src/tls_identity.cpp
  )
  target_include_directories(molink-core-quic-loopback-test PRIVATE src)
  target_compile_definitions(molink-core-quic-loopback-test PRIVATE
    MOLINK_HAVE_OPENSSL=1)
  if(UVP_UVCPP_PREFIX)
    target_include_directories(molink-core-quic-loopback-test PRIVATE
      "${UVP_UVCPP_PREFIX}/include")
    target_link_directories(molink-core-quic-loopback-test PRIVATE
      "${UVP_UVCPP_PREFIX}/lib")
  endif()
  target_link_libraries(molink-core-quic-loopback-test PRIVATE
    ${UVP_UVCPP_TARGET} OpenSSL::SSL OpenSSL::Crypto)
  if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
     CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(molink-core-quic-loopback-test PRIVATE stdc++fs)
  endif()
  set_target_properties(molink-core-quic-loopback-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    BUILD_RPATH "${UVP_UVCPP_LIBDIR}")
  foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${uvp_config}" uvp_config_upper)
    set_target_properties(molink-core-quic-loopback-test PROPERTIES
      "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
  endforeach()
  if(UNIX AND NOT APPLE)
    # libuvcpp.so needs libllhttp.so from its own directory and carries no
    # RUNPATH, so keep a legacy RPATH: unlike RUNPATH it is inherited by
    # indirect dependencies.
    target_link_options(molink-core-quic-loopback-test PRIVATE
      "LINKER:--disable-new-dtags")
  endif()
  add_test(NAME quic_loopback COMMAND molink-core-quic-loopback-test)
  set_tests_properties(quic_loopback PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Slow-receiver pressure on the real relay transport: a loopback WebSocket
# server stops reading while file traffic is pushed, and the transport has to
# apply backpressure instead of growing the client's send queue.
if(MOLINK_BUILD_CORE AND OpenSSL_FOUND)
  add_executable(molink-core-relay-channel-pressure-test
    tests/relay_channel_pressure_test.cpp
    src/transport/relay_transport.cpp
    src/transport/channel_scheduler.cpp
    src/protocol/frame_packet.cpp
  )
  target_include_directories(molink-core-relay-channel-pressure-test PRIVATE src)
  if(UVP_UVCPP_PREFIX)
    target_include_directories(molink-core-relay-channel-pressure-test PRIVATE
      "${UVP_UVCPP_PREFIX}/include")
    target_link_directories(molink-core-relay-channel-pressure-test PRIVATE
      "${UVP_UVCPP_PREFIX}/lib")
  endif()
  target_link_libraries(molink-core-relay-channel-pressure-test PRIVATE
    ${UVP_UVCPP_TARGET} OpenSSL::SSL OpenSSL::Crypto)
  set_target_properties(molink-core-relay-channel-pressure-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    BUILD_RPATH "${UVP_UVCPP_LIBDIR}")
  foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${uvp_config}" uvp_config_upper)
    set_target_properties(molink-core-relay-channel-pressure-test PROPERTIES
      "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
  endforeach()
  if(UNIX AND NOT APPLE)
    target_link_options(molink-core-relay-channel-pressure-test PRIVATE
      "LINKER:--disable-new-dtags")
  endif()
  add_test(NAME relay_channel_pressure
    COMMAND molink-core-relay-channel-pressure-test)
  set_tests_properties(relay_channel_pressure PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Relay endpoint fallback is pure sequencing; it links libuvcpp only because
# the ladder owns the per-attempt timer.
if(MOLINK_BUILD_CORE)
  add_executable(molink-core-relay-ladder-test
    tests/relay_ladder_test.cpp
    src/transport/relay_ladder.cpp
  )
  target_include_directories(molink-core-relay-ladder-test PRIVATE src)
  if(UVP_UVCPP_PREFIX)
    target_include_directories(molink-core-relay-ladder-test PRIVATE
      "${UVP_UVCPP_PREFIX}/include")
    target_link_directories(molink-core-relay-ladder-test PRIVATE
      "${UVP_UVCPP_PREFIX}/lib")
  endif()
  target_link_libraries(molink-core-relay-ladder-test PRIVATE
    ${UVP_UVCPP_TARGET})
  set_target_properties(molink-core-relay-ladder-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    BUILD_RPATH "${UVP_UVCPP_LIBDIR}")
  foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${uvp_config}" uvp_config_upper)
    set_target_properties(molink-core-relay-ladder-test PROPERTIES
      "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
  endforeach()
  if(UNIX AND NOT APPLE)
    target_link_options(molink-core-relay-ladder-test PRIVATE
      "LINKER:--disable-new-dtags")
  endif()
  add_test(NAME relay_ladder COMMAND molink-core-relay-ladder-test)
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
  if(TARGET molink-core-quic-loopback-test)
    target_compile_options(molink-core-quic-loopback-test PRIVATE
      /W4 /permissive- /utf-8)
  endif()
  if(TARGET molink-core-relay-ladder-test)
    target_compile_options(molink-core-relay-ladder-test PRIVATE
      /W4 /permissive- /utf-8)
  endif()
  if(TARGET molink-core-av1-test)
    target_compile_options(molink-core-av1-test PRIVATE /W4 /permissive- /utf-8)
  endif()
  if(TARGET molink-core-av1-frame-test)
    target_compile_options(molink-core-av1-frame-test PRIVATE
      /W4 /permissive- /utf-8)
  endif()
  if(TARGET molink-core-frame-encode-av1-test)
    target_compile_options(molink-core-frame-encode-av1-test PRIVATE
      /W4 /permissive- /utf-8)
  endif()
else()
  if(MOLINK_BUILD_CORE)
    target_compile_options(molink-core PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_compile_options(molink-core-protocol-test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(molink-core-dirty-rects-test PRIVATE -Wall -Wextra -Wpedantic)
  if(TARGET molink-core-quic-loopback-test)
    target_compile_options(molink-core-quic-loopback-test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  if(TARGET molink-core-relay-ladder-test)
    target_compile_options(molink-core-relay-ladder-test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  if(TARGET molink-core-av1-test)
    target_compile_options(molink-core-av1-test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(TARGET molink-core-av1-frame-test)
    target_compile_options(molink-core-av1-frame-test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  if(TARGET molink-core-frame-encode-av1-test)
    target_compile_options(molink-core-frame-encode-av1-test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()
