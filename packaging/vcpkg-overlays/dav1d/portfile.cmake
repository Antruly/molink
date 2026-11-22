# Overlay of the vcpkg dav1d port, pinned to 1.5.5.
#
# The runner's builtin port is 1.5.4, whose include/compat/msvc/stdatomic.h
# uses __has_include_next() without defining it; the MSVC on windows-latest
# rejects that with C7780. 1.5.5 guards the builtin (upstream commit "msvc:
# define __has_include_next"), which is the actual fix. 1.4.3 does NOT have
# that guard, so pinning backwards would fail the same way.
#
# Kept byte-for-byte identical to the upstream 1.5.5 port except for this
# comment, so the build recipe does not drift from vcpkg's.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO videolan/dav1d
    REF "${VERSION}"
    SHA512 1bd117c17b6cb0a40278d87c068e76bffef80baa196662b08711b9fa4b10b8ac37fecb2ba00c77cfac9c3d1d94eb3e942663f7bf0fa3e4168723e4d08d937152
    HEAD_REF master
)

if (VCPKG_TARGET_ARCHITECTURE STREQUAL "x86" OR VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    vcpkg_find_acquire_program(NASM)
    get_filename_component(NASM_EXE_PATH ${NASM} DIRECTORY)
    vcpkg_add_to_path(${NASM_EXE_PATH})
elseif (VCPKG_TARGET_IS_WINDOWS)
    vcpkg_find_acquire_program(GASPREPROCESSOR)
    foreach(GAS_PATH ${GASPREPROCESSOR})
        get_filename_component(GAS_ITEM_PATH ${GAS_PATH} DIRECTORY)
        vcpkg_add_to_path(${GAS_ITEM_PATH})
    endforeach(GAS_PATH)
endif()

set(LIBRARY_TYPE ${VCPKG_LIBRARY_LINKAGE})
if (LIBRARY_TYPE STREQUAL "dynamic")
    set(LIBRARY_TYPE "shared")
endif(LIBRARY_TYPE STREQUAL "dynamic")

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        --default-library=${LIBRARY_TYPE}
        -Denable_tests=false
        -Denable_tools=false
)

vcpkg_install_meson()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(
    FILE_LIST
        "${SOURCE_PATH}/COPYING"
        "${SOURCE_PATH}/src/ext/x86/x86inc.asm"
)
