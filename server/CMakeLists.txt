cmake_minimum_required(VERSION 3.20)
project(molink-server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# libuvcpp with WEBAPP (same dependency as molink-core).
find_package(uvcpp 1.7.0 CONFIG QUIET)
if(uvcpp_FOUND)
  set(UVP_UVCPP_TARGET uvcpp::uvcpp)
  get_target_property(_uvp_include_dirs uvcpp::uvcpp INTERFACE_INCLUDE_DIRECTORIES)
  if(_uvp_include_dirs)
    set(_uvp_clean_includes "")
    foreach(_dir ${_uvp_include_dirs})
      if(EXISTS "${_dir}")
        list(APPEND _uvp_clean_includes "${_dir}")
      endif()
    endforeach()
    get_filename_component(_uvp_prefix "${uvcpp_DIR}/../../.." ABSOLUTE)
    if(EXISTS "${_uvp_prefix}/include")
      list(APPEND _uvp_clean_includes "${_uvp_prefix}/include")
    endif()
    set_target_properties(uvcpp::uvcpp PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${_uvp_clean_includes}")
  endif()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(UVCPP REQUIRED IMPORTED_TARGET "uvcpp>=1.7.0")
  set(UVP_UVCPP_TARGET PkgConfig::UVCPP)
endif()

add_executable(molink-server
  src/main.cpp
  src/store.cpp
)

target_link_libraries(molink-server PRIVATE ${UVP_UVCPP_TARGET})

# libuvcpp's shared library needs libllhttp from its own directory and carries
# no RUNPATH of its own; keep an inherited legacy RPATH.
if(UNIX AND NOT APPLE)
  # Legacy RPATH is inherited by indirect dependencies; RUNPATH is not, and
  # libuvcpp.so needs libllhttp.so from the same prefix.
  target_link_options(molink-server PRIVATE "LINKER:--disable-new-dtags"
                                           "LINKER:-rpath,$ORIGIN"
                                           "LINKER:-rpath,$ORIGIN/../lib")
  set_target_properties(molink-server PROPERTIES
    INSTALL_RPATH "$ORIGIN:$ORIGIN/../lib")
endif()

install(TARGETS molink-server RUNTIME DESTINATION bin)
