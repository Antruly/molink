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
  src/directory_listing.cpp
  src/file_transfer.cpp
  src/port_forward.cpp
  src/portmap_api.cpp
  src/call_hub.cpp
  src/server_client.cpp
  src/audio/audio.cpp
  src/audio/windows_audio.cpp
  src/audio/windows_voice.cpp
  src/capture/capture.cpp
  src/capture/quality_profile.cpp
  src/capture/synthetic_capture.cpp
  src/capture/x11_capture.cpp
  src/capture/windows_capture.cpp
  src/capture/windows_gdi_capture.cpp
  src/capture/windows_secure_capture.cpp
  src/capture/capture_switch.cpp
  src/capture/frame_encode.cpp
  src/capture/pipeline_stats.cpp
  src/capture/dirty_rects.cpp
  src/capture/av1_encoder.cpp
  src/capture/vpx_stream_encoder.cpp
  src/capture/av1_frame.cpp
  src/surface/frame_surface.cpp
  src/surface/surface_compositor.cpp
  src/clipboard/x11_clipboard.cpp
  src/clipboard/windows_clipboard.cpp
  src/clipboard/windows_data_object.cpp
  src/clipboard/windows_virtual_clipboard.cpp
  src/clipboard/virtual_file_stream.cpp
  src/clipboard/clipboard_files.cpp
  src/clipboard/clipboard_image.cpp
  src/clipboard/clipboard_image_win.cpp
  src/clipboard/clipboard_route.cpp
  src/clipboard/x11_clipboard_fuse.cpp
  src/clipboard_stage.cpp
  src/clipboard_lazy.cpp
  src/clipboard_pull.cpp
  src/clipboard_virtual.cpp
  src/access_requests.cpp
  src/auth_state.cpp
  src/compression.cpp
  src/diag_log.cpp
  src/discovery.cpp
  src/settings.cpp
  src/totp.cpp
  src/ui_store.cpp
  src/input/x11_input.cpp
  src/input/windows_input.cpp
  src/input/windows_secure_input.cpp
  src/input/macos_input.cpp
  src/input/input_event.cpp
  src/input/key_alias.cpp
  src/input/keymap.cpp
  src/input/text_encoding.cpp
  src/config.cpp
  src/identity.cpp
  src/tls_identity.cpp
  src/lock_state.cpp
  src/net/ip_allowlist.cpp
  src/net/outbound_policy.cpp
  src/session_registry.cpp
  src/session_preset.cpp
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
  src/transport/port_forward_bridge.cpp
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
  src/terminal/pty.cpp
  src/terminal/terminal_service.cpp
  src/terminal/terminal_ws.cpp
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
    if(MOLINK_REQUIRE_DAV1D)
      message(FATAL_ERROR
        "MOLINK_REQUIRE_DAV1D=ON but dav1d was not found (DAV1D_INCLUDE_DIR=${DAV1D_INCLUDE_DIR}, DAV1D_LIBRARY=${DAV1D_LIBRARY}).")
    endif()
  endif()
endif()
if(MOLINK_REQUIRE_DAV1D AND NOT MOLINK_ENABLE_AV1)
  message(FATAL_ERROR "MOLINK_REQUIRE_DAV1D=ON needs MOLINK_ENABLE_AV1=ON")
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
# A packaging build that is supposed to ship the stream path must fail loudly
# when the SDK was not found: the resulting core silently stays tiles-only,
# which is exactly the "native shows no picture" report from Windows 7.
option(MOLINK_REQUIRE_VPX
  "Fail the configure step when libvpx is not found" OFF)
option(MOLINK_REQUIRE_DAV1D
  "Fail the configure step when dav1d (AV1 decode) is not found" OFF)
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
  elseif(MOLINK_REQUIRE_VPX)
    message(FATAL_ERROR
      "MOLINK_REQUIRE_VPX=ON but libvpx was not found (VPX_INCLUDE_DIR=${VPX_INCLUDE_DIR}, VPX_LIBRARY=${VPX_LIBRARY}). "
      "Point VPX_INCLUDE_DIR/VPX_LIBRARY at an unpacked libvpx tree.")
  else()
    message(STATUS "molink: libvpx not found; VP9/VP8 streams disabled")
  endif()
endif()
if(MOLINK_REQUIRE_VPX AND NOT MOLINK_ENABLE_VPX)
  message(FATAL_ERROR "MOLINK_REQUIRE_VPX=ON needs MOLINK_ENABLE_VPX=ON")
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

  # Windows: declare UTF-8 as the process ANSI code page. File names arrive as
  # UTF-8 and are converted explicitly at the std::filesystem boundary, but the
  # manifest also covers the C runtime and any narrow Win32 call left in the
  # clipboard/capture code. MSVC embeds the XML directly; MinGW compiles a
  # small resource script instead.
  if(WIN32)
    if(MSVC)
      target_link_options(molink-core PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/molink-core.manifest")
    else()
      target_sources(molink-core PRIVATE src/molink-core.rc)
      set_source_files_properties(src/molink-core.rc PROPERTIES
        COMPILE_FLAGS "-I\"${CMAKE_CURRENT_SOURCE_DIR}/src\"")
    endif()
  endif()

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

  # Virtual clipboard files (viewer side): remote clipboard entries become a
  # read-only FUSE mount, so copying transfers nothing and pasting pulls ranges
  # on demand. Optional: without libfuse3 the core falls back to downloading.
  set(MOLINK_HAVE_FUSE 0)
  if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(FUSE3 QUIET fuse3)
      if(FUSE3_FOUND)
        set(MOLINK_HAVE_FUSE 1)
      endif()
    endif()
  endif()

  if(WIN32)
    # crypt32: a static libcrypto's CAPI engine calls the Windows certificate
    # store APIs directly (MinGW builds against a static OpenSSL need it).
    target_link_libraries(molink-core PRIVATE
      d3d11 dxgi dxguid gdi32 user32 ole32 uuid ws2_32 iphlpapi advapi32
      shell32 crypt32 windowscodecs)
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

  if(MOLINK_HAVE_FUSE)
    target_compile_definitions(molink-core PRIVATE MOLINK_HAVE_FUSE=1)
    target_include_directories(molink-core PRIVATE ${FUSE3_INCLUDE_DIRS})
    target_link_directories(molink-core PRIVATE ${FUSE3_LIBRARY_DIRS})
    # Link the static archive when the distribution ships one. The SONAME of
    # the shared library is not stable across releases (3.14 exports
    # libfuse3.so.3, 3.16 libfuse3.so.4), so a package built on a newer runner
    # refuses to start on an older target with "libfuse3.so.4: cannot open
    # shared object file". The archive removes the dependency entirely.
    find_library(MOLINK_FUSE3_STATIC NAMES libfuse3.a fuse3
      PATHS ${FUSE3_LIBRARY_DIRS} NO_DEFAULT_PATH)
    if(MOLINK_FUSE3_STATIC AND MOLINK_FUSE3_STATIC MATCHES "\\.a$")
      target_link_libraries(molink-core PRIVATE ${MOLINK_FUSE3_STATIC})
      set(MOLINK_FUSE3_STATIC_LINKED TRUE)
      message(STATUS "molink: FUSE linked statically (${MOLINK_FUSE3_STATIC})")
    else()
      target_link_libraries(molink-core PRIVATE ${FUSE3_LIBRARIES})
      set(MOLINK_FUSE3_STATIC_LINKED FALSE)
      message(STATUS "molink: FUSE linked against libfuse3 at runtime")
    endif()
    message(STATUS "molink: FUSE virtual clipboard enabled")
  else()
    target_compile_definitions(molink-core PRIVATE MOLINK_HAVE_FUSE=0)
    message(STATUS "molink: libfuse3 not found; virtual clipboard unavailable")
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
  #
  # BEFORE matters: the archive bundles its own nlohmann/json.hpp and its
  # library was built against that copy, but vcpkg (pulled in by
  # libdatachannel) also installs one. If vcpkg's copy is compiled into our
  # objects, the ABI tag in every json type changes (v3_12_0 against the
  # archive's v3_11_3) and the link fails on uvcpp_json_dump/parse with no
  # hint as to why. Keep libuvcpp's headers in front.
  if(UVP_UVCPP_PREFIX)
    target_include_directories(molink-core BEFORE PRIVATE
      "${UVP_UVCPP_PREFIX}/include")
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

# CIDR parsing and matching for the IP allowlist. Pure libc (inet_pton), so it
# builds and runs without libuvcpp.
add_executable(molink-core-ip-allowlist-test
  tests/ip_allowlist_test.cpp
  src/net/ip_allowlist.cpp
)
target_include_directories(molink-core-ip-allowlist-test PRIVATE src)
if(WIN32)
  # inet_pton lives in Winsock.
  target_link_libraries(molink-core-ip-allowlist-test PRIVATE ws2_32)
endif()
set_target_properties(molink-core-ip-allowlist-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${uvp_config}" uvp_config_upper)
  set_target_properties(molink-core-ip-allowlist-test PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()
add_test(NAME ip_allowlist COMMAND molink-core-ip-allowlist-test)

# Host-only outbound policy: a normal install keeps every target, a host-only
# one permits loopback self-connections only, and the refusal carries the
# documented error code and log fields. Pure logic plus the logger.
add_executable(molink-core-outbound-policy-test
  tests/outbound_policy_test.cpp
  src/net/outbound_policy.cpp
  src/diag_log.cpp
)
target_include_directories(molink-core-outbound-policy-test PRIVATE src)
set_target_properties(molink-core-outbound-policy-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${uvp_config}" uvp_config_upper)
  set_target_properties(molink-core-outbound-policy-test PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()
add_test(NAME outbound_policy COMMAND molink-core-outbound-policy-test)

# TCP port forwarding: mapping persistence, a real local listener paired with a
# fake session bridge, byte counters, port release and the capability gate.
# The module uses plain BSD/Winsock sockets, so this target does not need
# libuvcpp.
add_executable(molink-core-port-forward-test
  tests/port_forward_test.cpp
  src/port_forward.cpp
  src/portmap_api.cpp
  src/transport/port_forward_bridge.cpp
  src/protocol/frame_packet.cpp
  src/diag_log.cpp
)
target_include_directories(molink-core-port-forward-test PRIVATE src)
if(WIN32)
  target_link_libraries(molink-core-port-forward-test PRIVATE ws2_32 iphlpapi)
endif()
if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
  target_link_libraries(molink-core-port-forward-test PRIVATE stdc++fs)
endif()
set_target_properties(molink-core-port-forward-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${uvp_config}" uvp_config_upper)
  set_target_properties(molink-core-port-forward-test PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()
add_test(NAME port_forward COMMAND molink-core-port-forward-test)

# Diagnostic log rotation, retention, tailing and redaction. Pure filesystem
# plus a fake clock, so it builds without libuvcpp.
add_executable(molink-core-diag-log-test
  tests/diag_log_test.cpp
  src/diag_log.cpp
)
target_include_directories(molink-core-diag-log-test PRIVATE src)
set_target_properties(molink-core-diag-log-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${uvp_config}" uvp_config_upper)
  set_target_properties(molink-core-diag-log-test PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()
add_test(NAME diag_log COMMAND molink-core-diag-log-test)

# Host PTY, the 1 MiB output ring and the session bookkeeping. The PTY calls
# are POSIX-only; the Windows stub compiles but the test is not registered
# there (ConPTY is a later batch).
add_executable(molink-core-terminal-test
  tests/terminal_test.cpp
  src/terminal/pty.cpp
  src/terminal/terminal_service.cpp
  src/diag_log.cpp
)
target_include_directories(molink-core-terminal-test PRIVATE src)
if(WIN32)
  target_link_libraries(molink-core-terminal-test PRIVATE ws2_32 iphlpapi)
endif()
set_target_properties(molink-core-terminal-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${uvp_config}" uvp_config_upper)
  set_target_properties(molink-core-terminal-test PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()
if(UNIX)
  add_test(NAME terminal COMMAND molink-core-terminal-test)
endif()

# The native client's diagnostics keep the same daily file name, retention,
# 32 MiB roll and redaction as the core, plus the level gate. No Win32: the
# Windows file calls are compiled only on Windows, and the Linux job exercises
# the POSIX path against the same behaviour.
add_executable(molink-core-native-diag-test
  tests/native_diag_test.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows/native_diag.cpp"
)
target_include_directories(molink-core-native-diag-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows")
set_target_properties(molink-core-native-diag-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${uvp_config}" uvp_config_upper)
  set_target_properties(molink-core-native-diag-test PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()
add_test(NAME native_diag COMMAND molink-core-native-diag-test)

# Which failures reach the session banner and which belong to a toast. Pure
# headers: the native shell compiles the same source into its Windows build.
add_executable(molink-core-error-policy-test
  tests/error_policy_test.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows/error_policy.cpp"
)
target_include_directories(molink-core-error-policy-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows")
set_target_properties(molink-core-error-policy-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME error_policy COMMAND molink-core-error-policy-test)

# The disconnected overlay's countdown must use the Web's retry schedule.
add_executable(molink-core-reconnect-policy-test
  tests/reconnect_policy_test.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows/reconnect_policy.cpp"
)
target_include_directories(molink-core-reconnect-policy-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows")
set_target_properties(molink-core-reconnect-policy-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME reconnect_policy COMMAND molink-core-reconnect-policy-test)

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

# Absolute-deadline capture pacing: no drift by encode time, no catch-up burst.
add_executable(molink-core-frame-schedule-test
  tests/frame_schedule_test.cpp
)
target_include_directories(molink-core-frame-schedule-test PRIVATE src)
set_target_properties(molink-core-frame-schedule-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME frame_schedule COMMAND molink-core-frame-schedule-test)

# Audio hub: silence gate, integer downmix and the shared per-packet buffer.
# A fake source and an injected clock keep this independent of libuvcpp and of
# a real WASAPI device.
add_executable(molink-core-audio-test
  tests/audio_test.cpp
  src/audio/audio.cpp
)
target_include_directories(molink-core-audio-test PRIVATE src)
set_target_properties(molink-core-audio-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${uvp_config}" uvp_config_upper)
  set_target_properties(molink-core-audio-test PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()
add_test(NAME audio COMMAND molink-core-audio-test)

# The media pipeline window: injected clock, fake samples, and the exact
# percentiles/counters the video.pipeline line reports.
add_executable(molink-core-pipeline-stats-test
  tests/pipeline_stats_test.cpp
  src/capture/pipeline_stats.cpp
)
target_include_directories(molink-core-pipeline-stats-test PRIVATE src)
set_target_properties(molink-core-pipeline-stats-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME pipeline_stats COMMAND molink-core-pipeline-stats-test)

# Input event validation and the legacy key aliases. Pure platform-neutral
# code: a fake injector checks every rejected path still carries a message.
add_executable(molink-core-input-event-test
  tests/input_event_test.cpp
  src/input/input_event.cpp
  src/input/key_alias.cpp
  src/input/keymap.cpp
)
target_include_directories(molink-core-input-event-test PRIVATE src)
set_target_properties(molink-core-input-event-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${uvp_config}" uvp_config_upper)
  set_target_properties(molink-core-input-event-test PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()
add_test(NAME input_event COMMAND molink-core-input-event-test)

# Every KeyboardEvent.code the host understands, resolved for all three
# platforms. Pure data: the Windows table is checked against the real VK_
# constants on Windows, and the same table is exercised on Linux so a missing
# entry fails CI before it reaches a user's keypad.
add_executable(molink-core-keymap-test
  tests/keymap_test.cpp
  src/input/keymap.cpp
  src/input/key_alias.cpp
  src/input/text_encoding.cpp
)
target_include_directories(molink-core-keymap-test PRIVATE src)
set_target_properties(molink-core-keymap-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${uvp_config}" uvp_config_upper)
  set_target_properties(molink-core-keymap-test PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()
add_test(NAME keymap COMMAND molink-core-keymap-test)

# Box averaging for large tile strips: pure pixel arithmetic.
# Clipboard staging for "remote file -> local clipboard": file naming, offset
# rules and the apply call. No libuvcpp or clipboard backend is linked; the
# test supplies a fake clipboard_service.
add_executable(molink-core-clipboard-stage-test
  tests/clipboard_stage_test.cpp
  src/clipboard_stage.cpp
)
target_include_directories(molink-core-clipboard-stage-test PRIVATE src)
set_target_properties(molink-core-clipboard-stage-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME clipboard_stage COMMAND molink-core-clipboard-stage-test)

# Host -> viewer clipboard file ranges: id/order validation, "the clipboard
# changed" (status 2), chunk bounds and short-read handling. Platform-neutral;
# the caller supplies the list that would have come from CF_HDROP.
add_executable(molink-core-clipboard-pull-test
  tests/clipboard_pull_test.cpp
  src/clipboard_pull.cpp
)
target_include_directories(molink-core-clipboard-pull-test PRIVATE src)
set_target_properties(molink-core-clipboard-pull-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME clipboard_pull COMMAND molink-core-clipboard-pull-test)

# CF_HDROP enumeration -> clipboard_file list. The Windows drag-and-drop API
# itself is not portable; the mapping, invalid-entry handling and the
# "session layer owns the caps" rule are.
add_executable(molink-core-clipboard-files-test
  tests/clipboard_files_test.cpp
  src/clipboard/clipboard_files.cpp
)
target_include_directories(molink-core-clipboard-files-test PRIVATE src)
set_target_properties(molink-core-clipboard-files-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME clipboard_files COMMAND molink-core-clipboard-files-test)

# Virtual clipboard lifecycle: offer caps and naming, mount/unmount on a new
# announcement, bridge relay and the read-failure path. The FUSE layer itself
# is not linked; the test supplies a fake mount factory.
add_executable(molink-core-clipboard-virtual-test
  tests/clipboard_virtual_test.cpp
  src/clipboard_virtual.cpp
  src/clipboard_stage.cpp
)
target_include_directories(molink-core-clipboard-virtual-test PRIVATE src)
set_target_properties(molink-core-clipboard-virtual-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME clipboard_virtual COMMAND molink-core-clipboard-virtual-test)

# The viewer-side IStream rules behind a virtual offer: 256 KiB range cap,
# EOF/short-read handling, seek arithmetic and failure propagation. COM is not
# involved, so this runs on every platform, including the Windows data object's
# byte accounting on Linux CI.
add_executable(molink-core-virtual-file-stream-test
  tests/virtual_file_stream_test.cpp
  src/clipboard/virtual_file_stream.cpp
)
target_include_directories(molink-core-virtual-file-stream-test PRIVATE src)
set_target_properties(molink-core-virtual-file-stream-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME virtual_file_stream
  COMMAND molink-core-virtual-file-stream-test)

# Delayed clipboard rendering state machine: offer limits, per-session clear,
# and the 15 s / 10 s render budget. Platform-neutral, so Windows-only delayed
# rendering is still covered by the Linux CI build.
add_executable(molink-core-clipboard-lazy-test
  tests/clipboard_lazy_test.cpp
  src/clipboard_lazy.cpp
)
target_include_directories(molink-core-clipboard-lazy-test PRIVATE src)
set_target_properties(molink-core-clipboard-lazy-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME clipboard_lazy COMMAND molink-core-clipboard-lazy-test)

# Windows-only: takes the OLE clipboard, then reads it back as Explorer does
# (FileGroupDescriptorW + FileContents IStream). It is skipped unless
# MOLINK_CLIPBOARD_WINDOWS_TEST=1 because it replaces the developer's clipboard.
# The fetch table behind it is covered by clipboard_lazy_test on every OS.
# X11 file clipboard writer: takes CLIPBOARD and asks the selection for
# text/uri-list and x-special/gnome-copied-files the way a file manager does.
# It replaces the developer's clipboard, so it is gated on an environment
# variable just like the Windows test.
if(MOLINK_HAVE_X11)
  add_executable(molink-core-x11-clipboard-files-test
    tests/x11_clipboard_files_test.cpp
    src/clipboard/x11_clipboard.cpp
    src/clipboard/clipboard_files.cpp
    src/clipboard/clipboard_image.cpp
    src/clipboard/clipboard_route.cpp
    src/diag_log.cpp
    src/util.cpp
  )
  target_include_directories(molink-core-x11-clipboard-files-test PRIVATE src)
  target_compile_definitions(molink-core-x11-clipboard-files-test
    PRIVATE MOLINK_HAVE_X11=1)
  target_link_libraries(molink-core-x11-clipboard-files-test PRIVATE
    X11::X11 pthread)
  set_target_properties(molink-core-x11-clipboard-files-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  add_test(NAME x11_clipboard_files
    COMMAND molink-core-x11-clipboard-files-test)
  set_tests_properties(x11_clipboard_files PROPERTIES
    SKIP_RETURN_CODE 77)
endif()

if(WIN32)
  add_executable(molink-core-windows-clipboard-stream-test
    tests/windows_clipboard_stream_test.cpp
    src/clipboard/windows_clipboard.cpp
    src/clipboard/windows_data_object.cpp
    src/clipboard/windows_virtual_clipboard.cpp
    src/clipboard/virtual_file_stream.cpp
    src/clipboard/clipboard_files.cpp
    src/clipboard/clipboard_image.cpp
    src/clipboard/clipboard_image_win.cpp
    src/diag_log.cpp
    src/util.cpp
    src/clipboard_lazy.cpp
  )
  target_include_directories(molink-core-windows-clipboard-stream-test
    PRIVATE src)
  target_link_libraries(molink-core-windows-clipboard-stream-test
    PRIVATE ole32 oleaut32 uuid shell32 user32 gdi32 windowscodecs
            ws2_32 iphlpapi)
  set_target_properties(molink-core-windows-clipboard-stream-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  add_test(NAME windows_clipboard_stream
    COMMAND molink-core-windows-clipboard-stream-test)
  set_tests_properties(windows_clipboard_stream PROPERTIES
    SKIP_RETURN_CODE 77)

  # CF_DIB <-> PNG behind the image clipboard. WIC is a system component, and
  # this test never opens the real clipboard, so it runs unconditionally.
  add_executable(molink-core-windows-clipboard-image-test
    tests/windows_clipboard_image_test.cpp
    src/clipboard/clipboard_image.cpp
    src/clipboard/clipboard_image_win.cpp
  )
  target_include_directories(molink-core-windows-clipboard-image-test
    PRIVATE src)
  target_link_libraries(molink-core-windows-clipboard-image-test
    PRIVATE ole32 uuid windowscodecs)
  set_target_properties(molink-core-windows-clipboard-image-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  add_test(NAME windows_clipboard_image
    COMMAND molink-core-windows-clipboard-image-test)

  # Reads a fake IDataObject exactly the way the host reads mstsc's nested-RDP
  # offer: FileGroupDescriptorW first, FileContents streams second, and
  # CF_HDROP only when the stream formats are absent. It never touches the
  # real clipboard, so it runs unconditionally on Windows.
  add_executable(molink-core-windows-data-object-test
    tests/windows_data_object_test.cpp
    src/clipboard/windows_data_object.cpp
  )
  target_include_directories(molink-core-windows-data-object-test PRIVATE src)
  target_link_libraries(molink-core-windows-data-object-test
    PRIVATE ole32 oleaut32 uuid)
  set_target_properties(molink-core-windows-data-object-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  add_test(NAME windows_data_object
    COMMAND molink-core-windows-data-object-test)

  # The viewer-side counterpart: the data object that publishes a remote
  # clipboard selection as FileContents streams. It never touches the real
  # clipboard, so it runs unconditionally on Windows.
  add_executable(molink-core-windows-virtual-object-test
    tests/windows_virtual_clipboard_test.cpp
    src/clipboard/windows_virtual_clipboard.cpp
    src/clipboard/windows_data_object.cpp
    src/clipboard/virtual_file_stream.cpp
  )
  target_include_directories(molink-core-windows-virtual-object-test
    PRIVATE src)
  target_link_libraries(molink-core-windows-virtual-object-test
    PRIVATE ole32 oleaut32 uuid shell32 user32)
  set_target_properties(molink-core-windows-virtual-object-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  add_test(NAME windows_virtual_object
    COMMAND molink-core-windows-virtual-object-test)

  # The same offer through the real clipboard: STA thread, message loop,
  # OleSetClipboard and a read back via OleGetClipboard. Gated because it
  # replaces the developer's clipboard.
  add_executable(molink-core-windows-virtual-install-test
    tests/windows_virtual_clipboard_install_test.cpp
    src/clipboard/windows_virtual_clipboard.cpp
    src/clipboard/windows_data_object.cpp
    src/clipboard/virtual_file_stream.cpp
  )
  target_include_directories(molink-core-windows-virtual-install-test
    PRIVATE src)
  target_link_libraries(molink-core-windows-virtual-install-test
    PRIVATE ole32 oleaut32 uuid shell32 user32)
  set_target_properties(molink-core-windows-virtual-install-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  add_test(NAME windows_virtual_install
    COMMAND molink-core-windows-virtual-install-test)
  set_tests_properties(windows_virtual_install PROPERTIES
    SKIP_RETURN_CODE 77)
endif()

add_executable(molink-core-tile-scale-test
  tests/tile_scale_test.cpp
)
target_include_directories(molink-core-tile-scale-test PRIVATE src)
set_target_properties(molink-core-tile-scale-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME tile_scale COMMAND molink-core-tile-scale-test)

# RFC 6238 vectors and the sealed-at-rest secret: pure crypto, no libuvcpp.
add_executable(molink-core-totp-test
  tests/totp_test.cpp
  src/totp.cpp
)
target_include_directories(molink-core-totp-test PRIVATE src)
set_target_properties(molink-core-totp-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME totp COMMAND molink-core-totp-test)

# The capability presets are pure data, so the refusal matrix is checked
# without a transport.
add_executable(molink-core-capability-test
  tests/capability_test.cpp
)
target_include_directories(molink-core-capability-test PRIVATE src)
set_target_properties(molink-core-capability-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME capability COMMAND molink-core-capability-test)

# Connection budgets and per-session capability overrides. The fake transport
# keeps this independent of libuvcpp; util.cpp supplies the session id.
add_executable(molink-core-session-registry-test
  tests/session_registry_test.cpp
  src/session_registry.cpp
  src/util.cpp
)
target_include_directories(molink-core-session-registry-test PRIVATE src)
if(WIN32)
  target_link_libraries(molink-core-session-registry-test PRIVATE
    ws2_32 iphlpapi)
endif()
set_target_properties(molink-core-session-registry-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME session_registry COMMAND molink-core-session-registry-test)

# The tile <-> AV1 stream hand-over is pure state machine.
add_executable(molink-core-stream-policy-test
  tests/stream_policy_test.cpp
)
target_include_directories(molink-core-stream-policy-test PRIVATE src)
set_target_properties(molink-core-stream-policy-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME stream_policy COMMAND molink-core-stream-policy-test)

# Video path selection: auto / tiles / stream, and what the welcome offers.
add_executable(molink-core-video-mode-test
  tests/video_mode_test.cpp
)
target_include_directories(molink-core-video-mode-test PRIVATE src)
set_target_properties(molink-core-video-mode-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME video_mode COMMAND molink-core-video-mode-test)

# Viewer-owned picture presets: validation, plan defaults and the
# "rebuild forces a keyframe" contract.
add_executable(molink-core-session-preset-test
  tests/session_preset_test.cpp
  src/session_preset.cpp
  src/capture/quality_profile.cpp
)
target_include_directories(molink-core-session-preset-test PRIVATE src)
target_compile_definitions(molink-core-session-preset-test PRIVATE
  MOLINK_SESSION_PRESET_TEST=1)
set_target_properties(molink-core-session-preset-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME session_preset COMMAND molink-core-session-preset-test)

# The native shell's session-preset payload is built by a Windows-independent
# source, so the exact bytes the viewer puts on the wire are tested here on
# Linux (and in the Windows protocol job) without a display or Win32.
add_executable(molink-core-preset-message-test
  tests/preset_message_test.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows/preset_message.cpp"
)
target_include_directories(molink-core-preset-message-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows")
if(MSVC)
  # The tier labels and the expected JSON are UTF-8 source text.
  target_compile_options(molink-core-preset-message-test PRIVATE /utf-8)
endif()
set_target_properties(molink-core-preset-message-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME preset_message COMMAND molink-core-preset-message-test)

# The native shell's string table: every key must carry both languages, and
# the language preference document the shell reads at startup round-trips.
# No Win32: the same source is compiled into the Windows client.
add_executable(molink-core-ui-strings-test
  tests/ui_strings_test.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows/ui_strings.cpp"
)
target_include_directories(molink-core-ui-strings-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows")
if(MSVC)
  # The table carries UTF-8 Chinese source text.
  target_compile_options(molink-core-ui-strings-test PRIVATE /utf-8)
endif()
set_target_properties(molink-core-ui-strings-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME ui_strings COMMAND molink-core-ui-strings-test)

# Watermark proximity geometry and the 28 px / 80 ms fade rules, shared with
# the Windows painter so they can be checked without a display.
add_executable(molink-core-watermark-fade-test
  tests/watermark_fade_test.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows/watermark_fade.cpp"
)
target_include_directories(molink-core-watermark-fade-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows")
set_target_properties(molink-core-watermark-fade-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME watermark_fade COMMAND molink-core-watermark-fade-test)

# Clipboard text sync without Win32: the empty/duplicate/192 KiB rules and the
# echo table (two-minute window, 64 entries) are shared with the native shell
# and tested here on Linux. The Windows calls themselves stay in ui_kit_demo.
add_executable(molink-core-clipboard-sync-test
  tests/clipboard_sync_test.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows/clipboard_sync.cpp"
)
target_include_directories(molink-core-clipboard-sync-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows")
set_target_properties(molink-core-clipboard-sync-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME clipboard_sync COMMAND molink-core-clipboard-sync-test)

# Clipboard coexistence: who owns the clipboard, which viewer is active, and
# the per-session echo table (so two connected viewers cannot suppress each
# other's genuine copies). Pure data; the platform owner calls live in the
# X11/Windows/macOS backends.
add_executable(molink-core-clipboard-route-test
  tests/clipboard_route_test.cpp
  src/clipboard/clipboard_route.cpp
  src/util.cpp
)
target_include_directories(molink-core-clipboard-route-test PRIVATE src)
if(WIN32)
  # util.cpp reaches Winsock/iphlpapi for local_ipv4_addresses.
  target_link_libraries(molink-core-clipboard-route-test PRIVATE ws2_32 iphlpapi)
endif()
set_target_properties(molink-core-clipboard-route-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${uvp_config}" uvp_config_upper)
  set_target_properties(molink-core-clipboard-route-test PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()
add_test(NAME clipboard_route COMMAND molink-core-clipboard-route-test)

# Clipboard image rules: the 8 MiB compressed cap, the de-duplication hash and
# the owner decision that keeps a self-written image from being announced as a
# fresh local copy. Pure data; platform conversions live in their own tests.
add_executable(molink-core-clipboard-image-test
  tests/clipboard_image_test.cpp
  src/clipboard/clipboard_image.cpp
)
target_include_directories(molink-core-clipboard-image-test PRIVATE src)
set_target_properties(molink-core-clipboard-image-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME clipboard_image COMMAND molink-core-clipboard-image-test)

# Clipboard *files* without Win32: the 8 / 1 TiB / 4 TiB pull caps and
# omitted count, the 256 KiB range boundaries, the staging path, the retry
# budget and the echo table are shared with the native shell and tested here.
add_executable(molink-core-native-clipboard-files-test
  tests/native_clipboard_files_test.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows/clipboard_files.cpp"
)
target_include_directories(molink-core-native-clipboard-files-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows")
set_target_properties(molink-core-native-clipboard-files-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME native_clipboard_files
         COMMAND molink-core-native-clipboard-files-test)

# File-panel rules without Win32: the path grammar (typing C:\Windows, ~,
# relative paths, drive/UNC crumbs), the Ctrl/Shift selection model and the
# per-task aggregation are shared with the native shell and tested here.
add_executable(molink-core-file-panel-logic-test
  tests/file_panel_logic_test.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows/file_panel_logic.cpp"
)
target_include_directories(molink-core-file-panel-logic-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows")
set_target_properties(molink-core-file-panel-logic-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME file_panel_logic COMMAND molink-core-file-panel-logic-test)

# Keyboard translation and the pressed-key state machine without Win32: the
# keypad/NumLock mapping, duplicate key-down filtering, blur release and the
# shortcut injection order are shared with the native shell and tested here.
add_executable(molink-core-input-state-test
  tests/input_state_test.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows/input_state.cpp"
)
target_include_directories(molink-core-input-state-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows")
set_target_properties(molink-core-input-state-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME input_state COMMAND molink-core-input-state-test)

# Automatic quality: the tier ladder, hysteresis/dwell and the load-aware
# frame rate are pure and shared with the native shell, so the thresholds the
# Web uses can be pinned on Linux.
add_executable(molink-core-adaptive-quality-test
  tests/adaptive_quality_test.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows/adaptive_quality.cpp"
)
target_include_directories(molink-core-adaptive-quality-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows")
set_target_properties(molink-core-adaptive-quality-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME adaptive_quality COMMAND molink-core-adaptive-quality-test)

# Multi-monitor selection: welcome.displays parsing and the "a real switch
# resets the decoder" rule are shared with the native shell.
add_executable(molink-core-display-selector-test
  tests/display_selector_test.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows/display_selector.cpp"
)
target_include_directories(molink-core-display-selector-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows")
if(MSVC)
  # The parsed UTF-8 monitor name is source text in the test.
  target_compile_options(molink-core-display-selector-test PRIVATE /utf-8)
endif()
set_target_properties(molink-core-display-selector-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME display_selector COMMAND molink-core-display-selector-test)

# Multi-monitor hotplug/resize/rotate: the layout comparison and the primary
# selection are pure, so fake lists drive each case without a display server.
add_executable(molink-core-display-layout-test
  tests/display_layout_test.cpp
)
target_include_directories(molink-core-display-layout-test PRIVATE src)
set_target_properties(molink-core-display-layout-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER ${uvp_config} uvp_config_upper)
  set_target_properties(molink-core-display-layout-test PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()
add_test(NAME display_layout COMMAND molink-core-display-layout-test)

# Linux desktop session classification. Pure environment parsing: a Wayland
# host can state that only XWayland applications are reachable instead of
# silently degrading, and every variable combination is covered here.
add_executable(molink-core-session-type-test
  tests/session_type_test.cpp
)
target_include_directories(molink-core-session-type-test PRIVATE src)
set_target_properties(molink-core-session-type-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER ${uvp_config} uvp_config_upper)
  set_target_properties(molink-core-session-type-test PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()
add_test(NAME session_type COMMAND molink-core-session-type-test)

# Relative (trackpad / virtual mouse) movement and sensitivity conversion.
add_executable(molink-core-trackpad-mouse-test
  tests/trackpad_mouse_test.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows/trackpad_mouse.cpp"
)
target_include_directories(molink-core-trackpad-mouse-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../shell/windows")
set_target_properties(molink-core-trackpad-mouse-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME trackpad_mouse COMMAND molink-core-trackpad-mouse-test)

# Service registration: the path staleness rule is pure and shared with the
# status endpoint (Win32/systemd orchestration stays in the core).
add_executable(molink-core-service-control-test
  tests/service_control_test.cpp
  src/service_control.cpp
)
target_include_directories(molink-core-service-control-test PRIVATE src)
if(WIN32)
  target_link_libraries(molink-core-service-control-test PRIVATE
    advapi32 shell32 wtsapi32)
endif()
set_target_properties(molink-core-service-control-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME service_control COMMAND molink-core-service-control-test)

# The weight-based QoS controller: viewer feedback in, budget scale out.
add_executable(molink-core-stream-qos-test
  tests/stream_qos_test.cpp
)
target_include_directories(molink-core-stream-qos-test PRIVATE src)
set_target_properties(molink-core-stream-qos-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME stream_qos COMMAND molink-core-stream-qos-test)

# The native picture path: tiles compose into the shell's shared surface.
add_executable(molink-core-surface-test
  tests/surface_compositor_test.cpp
  src/surface/frame_surface.cpp
  src/surface/surface_compositor.cpp
  src/capture/av1_frame.cpp
  src/protocol/frame_packet.cpp
)
target_include_directories(molink-core-surface-test PRIVATE src)
if(MOLINK_HAVE_ZLIB)
  target_compile_definitions(molink-core-surface-test PRIVATE MOLINK_HAVE_ZLIB=1)
  target_link_libraries(molink-core-surface-test PRIVATE ZLIB::ZLIB)
endif()
set_target_properties(molink-core-surface-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME surface_compositor COMMAND molink-core-surface-test)

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

# A bounded TCP queue may drop video but must never drop reliable traffic.
add_executable(molink-core-tcp-backlog-test
  tests/tcp_backlog_test.cpp
)
target_include_directories(molink-core-tcp-backlog-test PRIVATE src)
set_target_properties(molink-core-tcp-backlog-test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME tcp_backlog COMMAND molink-core-tcp-backlog-test)

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
    src/directory_listing.cpp
    src/diag_log.cpp
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

  # Path normalisation, volume enumeration and the "any folder" listing. The
  # path rules are pure string code, so the Windows cases run on every host.
  add_executable(molink-core-file-browse-test
    tests/file_browse_test.cpp
    src/file_transfer.cpp
    src/directory_listing.cpp
    src/diag_log.cpp
    src/util.cpp
  )
  target_include_directories(molink-core-file-browse-test PRIVATE
    src ${UVP_UVCPP_PREFIX}/include)
  if(WIN32)
    target_link_libraries(molink-core-file-browse-test PRIVATE
      ws2_32 iphlpapi)
  endif()
  set_target_properties(molink-core-file-browse-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  add_test(NAME file_browse COMMAND molink-core-file-browse-test)
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
  if(TARGET molink-core-display-layout-test)
    target_compile_options(molink-core-display-layout-test PRIVATE
      /W4 /permissive- /utf-8)
  endif()
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
  if(TARGET molink-core-display-layout-test)
    target_compile_options(molink-core-display-layout-test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
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
