# Cross/native build for Windows 7 x86 with the libuvcpp MinGW32 kit.
#
#   MOLINK_WIN7_KIT=/path/to/libuvcpp-1.8.3-mingw32 \
#     cmake -S backend -B build-win7x86 -G "MinGW Makefiles" \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw32-win7.cmake \
#       -DCMAKE_PREFIX_PATH="$MOLINK_WIN7_KIT/prebuilt" \
#       -DMOLINK_CXX_STANDARD=17 -DCMAKE_BUILD_TYPE=Release
#
# The kit is uvcpp-kit-1.8.3-mingw32.zip from the libuvcpp releases. It
# already contains GCC 8.1 (deps/mingw32), libuvcpp (prebuilt/), zlib and
# OpenSSL, all built for i686 Windows 7.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

if(NOT MOLINK_WIN7_KIT)
  if(DEFINED ENV{MOLINK_WIN7_KIT})
    set(MOLINK_WIN7_KIT "$ENV{MOLINK_WIN7_KIT}")
  else()
    message(FATAL_ERROR "Set MOLINK_WIN7_KIT to the extracted MinGW32 kit")
  endif()
endif()
file(TO_CMAKE_PATH "${MOLINK_WIN7_KIT}" MOLINK_WIN7_KIT)

set(CMAKE_C_COMPILER   "${MOLINK_WIN7_KIT}/deps/mingw32/bin/gcc.exe")
set(CMAKE_CXX_COMPILER "${MOLINK_WIN7_KIT}/deps/mingw32/bin/g++.exe")
set(CMAKE_RC_COMPILER  "${MOLINK_WIN7_KIT}/deps/mingw32/bin/windres.exe")
set(CMAKE_MAKE_PROGRAM "${MOLINK_WIN7_KIT}/deps/mingw32/bin/mingw32-make.exe")
set(CMAKE_AR           "${MOLINK_WIN7_KIT}/deps/mingw32/bin/ar.exe")
set(CMAKE_RANLIB       "${MOLINK_WIN7_KIT}/deps/mingw32/bin/ranlib.exe")

set(CMAKE_FIND_ROOT_PATH
  "${MOLINK_WIN7_KIT}/deps/mingw32/i686-w64-mingw32"
  "${MOLINK_WIN7_KIT}/prebuilt"
  "${MOLINK_WIN7_KIT}/deps/zlib"
  "${MOLINK_WIN7_KIT}/deps/openssl")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_FLAGS_INIT   "-D_WIN32_WINNT=0x0601 -DWINVER=0x0601")
set(CMAKE_CXX_FLAGS_INIT "-D_WIN32_WINNT=0x0601 -DWINVER=0x0601")
