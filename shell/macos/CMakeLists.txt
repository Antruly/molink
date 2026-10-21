cmake_minimum_required(VERSION 3.20)
project(moLinkMac LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)

add_executable(moLink main.mm)
target_compile_options(moLink PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)

find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
find_library(WEBKIT_FRAMEWORK WebKit REQUIRED)
target_link_libraries(moLink PRIVATE ${COCOA_FRAMEWORK} ${WEBKIT_FRAMEWORK})

# The .app bundle is assembled by packaging/macos/build-dmg.sh; the executable
# looks for the core next to itself and for dylibs at @loader_path.
set_target_properties(moLink PROPERTIES
  BUILD_RPATH ""
  INSTALL_RPATH "@executable_path"
  MACOSX_RPATH TRUE
)

install(TARGETS moLink RUNTIME DESTINATION .)
