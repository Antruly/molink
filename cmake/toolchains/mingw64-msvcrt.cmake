# Native Windows x64 build with the MSVCRT MinGW-w64 toolchain (the MSYS2
# `mingw64` repository). This is the x64 counterpart of mingw32-win7.cmake:
# the same runtime (msvcrt.dll) the Windows 7 x86 kit ships, so the binaries
# run on a stock Windows 7 SP1 without the UCRT update (KB2999226).
#
#   MOLINK_MINGW64_PREFIX=/c/msys64/mingw64 \
#     cmake -S backend -B build-mingw64 -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw64-msvcrt.cmake \
#       -DMOLINK_UVCPP_PREFIX=/path/to/libuvcpp-1.9.1-mingw-x64
#
# MOLINK_MINGW64_PREFIX must contain the compiler and the dependency
# libraries as installed by MSYS2:
#
#   pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-openssl \
#     mingw-w64-x86_64-zlib mingw-w64-x86_64-libvpx mingw-w64-x86_64-dav1d \
#     mingw-w64-x86_64-svt-av1 mingw-w64-x86_64-usrsctp \
#     mingw-w64-x86_64-nlohmann-json
#
# Do not point it at an `ucrt64` prefix. The libuvcpp mingw-x64 release
# archive links msvcrt.dll, and its public API passes std::string /
# std::function across the DLL boundary; mixing it with a UCRT build puts two
# CRTs and two heaps in one process, which is undefined behaviour. Build
# libuvcpp from source with this same toolchain and UVCPP_STATIC_RUNTIME=ON
# if a prebuilt archive for this exact toolchain is not available.

if(NOT MOLINK_MINGW64_PREFIX)
  if(DEFINED ENV{MOLINK_MINGW64_PREFIX})
    set(MOLINK_MINGW64_PREFIX "$ENV{MOLINK_MINGW64_PREFIX}")
  else()
    message(FATAL_ERROR
      "Set MOLINK_MINGW64_PREFIX to the MSYS2 mingw64 prefix")
  endif()
endif()
file(TO_CMAKE_PATH "${MOLINK_MINGW64_PREFIX}" MOLINK_MINGW64_PREFIX)

# This is a native build (the compiler runs on the host), so no
# CMAKE_SYSTEM_NAME/CMAKE_FIND_ROOT_PATH restrictions are set: dependencies
# such as libdatachannel may live outside the toolchain prefix and are passed
# through CMAKE_PREFIX_PATH like on any other platform.

# The toolchain is native (it runs on the host), so no cross-compiler prefix
# splitting is needed beyond the compiler names themselves.
set(_molink_mingw64_bin "${MOLINK_MINGW64_PREFIX}/bin")
if(NOT EXISTS "${_molink_mingw64_bin}/x86_64-w64-mingw32-g++.exe")
  message(FATAL_ERROR
    "MOLINK_MINGW64_PREFIX does not look like an MSYS2 mingw64 prefix: "
    "${_molink_mingw64_bin}/x86_64-w64-mingw32-g++.exe is missing")
endif()

set(CMAKE_C_COMPILER   "${_molink_mingw64_bin}/x86_64-w64-mingw32-gcc.exe")
set(CMAKE_CXX_COMPILER "${_molink_mingw64_bin}/x86_64-w64-mingw32-g++.exe")
set(CMAKE_RC_COMPILER  "${_molink_mingw64_bin}/windres.exe")
set(CMAKE_AR           "${_molink_mingw64_bin}/ar.exe")
set(CMAKE_RANLIB       "${_molink_mingw64_bin}/ranlib.exe")

# Same Windows baseline as the shells and the MSVC core.
set(CMAKE_C_FLAGS_INIT   "-D_WIN32_WINNT=0x0A00")
set(CMAKE_CXX_FLAGS_INIT "-D_WIN32_WINNT=0x0A00")
