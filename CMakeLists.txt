cmake_minimum_required(VERSION 3.28)
project(obsidian-cpp VERSION 0.1.0 DESCRIPTION "Native C++/Qt clone of Obsidian" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

option(OC_BUILD_TESTS "Build unit/UI tests" ON)
option(OC_WITH_MATH "Build JKQTMathText (native LaTeX math rendering)" ON)
set(OC_SANITIZE "" CACHE STRING "Semicolon list of sanitizers (address;undefined | thread)")

include(OcToolchain)
include(OcTarget)

find_package(Qt6 6.8 REQUIRED COMPONENTS
  Core Gui Widgets Svg SvgWidgets Concurrent PrintSupport Pdf PdfWidgets
  Multimedia MultimediaWidgets Test)
qt_standard_project_setup(REQUIRES 6.8)

find_package(KF6SyntaxHighlighting REQUIRED)
find_package(KF6Sonnet REQUIRED)
find_package(KF6KIO REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(zstd REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)

if(OC_BUILD_TESTS)
  find_package(GTest REQUIRED)
  enable_testing()
endif()

add_subdirectory(third_party)
add_subdirectory(resources)
add_subdirectory(src)
if(OC_BUILD_TESTS)
  add_subdirectory(tests)
endif()
