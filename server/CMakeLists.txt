cmake_minimum_required(VERSION 3.20)
project(molink-server VERSION 0.2.0 LANGUAGES CXX)

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

# The admin console, accounts and security policy all live in the database;
# a libuvcpp without the DB module cannot build this server.
get_target_property(_uvp_definitions ${UVP_UVCPP_TARGET} INTERFACE_COMPILE_DEFINITIONS)
if(_uvp_definitions AND NOT "${_uvp_definitions}" MATCHES "UVCPP_DB_ENABLE=1")
  message(FATAL_ERROR
    "molink-server needs libuvcpp built with -DUVCPP_ENABLE_DB=ON "
    "(and the SQLite backend). Rebuild libuvcpp with: "
    "-DUVCPP_ENABLE_DB=ON -DUVCPP_ENABLE_DB_SQLITE=ON "
    "-DUVCPP_DB_SQLITE_FROM_SOURCE=ON")
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_library(molink_server_core STATIC
  src/util.cpp
  src/logging.cpp
  src/config.cpp
  src/db/database.cpp
  src/db/migrations.cpp
  src/db/repositories.cpp
  src/settings.cpp
  src/security/ip.cpp
  src/security/cn_ranges.cpp
  src/security/rate_limiter.cpp
  src/auth/password.cpp
)
target_include_directories(molink_server_core PUBLIC src)
target_link_libraries(molink_server_core PUBLIC
  ${UVP_UVCPP_TARGET}
  OpenSSL::Crypto
  Threads::Threads
)

add_executable(molink-server
  src/main.cpp
  src/store.cpp
)
target_link_libraries(molink-server PRIVATE molink_server_core)

install(TARGETS molink-server RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
  add_executable(molink-server-foundation-test tests/foundation_smoke.cpp)
  target_link_libraries(molink-server-foundation-test PRIVATE molink_server_core)
  add_test(NAME server-foundation COMMAND molink-server-foundation-test)

  add_executable(molink-server-unit-test tests/security_test.cpp)
  target_link_libraries(molink-server-unit-test PRIVATE molink_server_core)
  add_test(NAME server-security COMMAND molink-server-unit-test)

  add_executable(molink-server-repository-test tests/repository_test.cpp)
  target_link_libraries(molink-server-repository-test PRIVATE molink_server_core)
  add_test(NAME server-repositories COMMAND molink-server-repository-test)
endif()

# libuvcpp's shared library needs libllhttp from its own directory and carries
# no RUNPATH of its own; keep an inherited legacy RPATH. The development build
# also points at the libuvcpp prefix so the tests run without LD_LIBRARY_PATH.
if(UNIX AND NOT APPLE)
  set(_uvp_rpath "$ORIGIN:$ORIGIN/../lib")
  if(DEFINED _uvp_prefix AND EXISTS "${_uvp_prefix}/lib")
    string(APPEND _uvp_rpath ":${_uvp_prefix}/lib")
  endif()
  foreach(_exe molink-server molink-server-foundation-test
          molink-server-unit-test molink-server-repository-test)
    if(TARGET ${_exe})
      target_link_options(${_exe} PRIVATE "LINKER:--disable-new-dtags"
                                           "LINKER:-rpath,${_uvp_rpath}")
      set_target_properties(${_exe} PROPERTIES
        BUILD_RPATH "${_uvp_rpath}"
        INSTALL_RPATH "$ORIGIN:$ORIGIN/../lib")
    endif()
  endforeach()
endif()
