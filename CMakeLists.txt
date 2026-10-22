# Loserfish build for GCC / Clang (Linux, macOS, MinGW on Windows). MSVC is not supported.
# The Makefile in src/ needs a POSIX shell; this file does not.
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
# Options: -DLOSERFISH_NATIVE=ON uses -march=native (not portable), -DLOSERFISH_STATIC=ON links statically.
cmake_minimum_required(VERSION 3.10)
project(Loserfish CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOSERFISH_NATIVE "Build with -march=native (not portable)" OFF)
option(LOSERFISH_STATIC "Link statically" OFF)

set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)
set(SOURCES
  attacks.cpp benchmark.cpp bitboard.cpp evaluate.cpp main.cpp misc.cpp movegen.cpp movepick.cpp
  position.cpp search.cpp thread.cpp timeman.cpp tt.cpp uci.cpp ucioption.cpp tune.cpp
  syzygy/tbprobe.cpp engine.cpp score.cpp memory.cpp losesearch.cpp misere.cpp)
list(TRANSFORM SOURCES PREPEND ${SRC_DIR}/)

add_executable(loserfish ${SOURCES})
target_include_directories(loserfish PRIVATE ${SRC_DIR})
target_compile_options(loserfish PRIVATE -O3 -Wall -Wextra -pedantic)
target_compile_definitions(loserfish PRIVATE $<$<CONFIG:Release>:NDEBUG>)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  target_compile_definitions(loserfish PRIVATE IS_64BIT)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86|x86)$")
  target_compile_definitions(loserfish PRIVATE USE_SSE2)
  target_compile_options(loserfish PRIVATE -msse -msse2)
  if(LOSERFISH_NATIVE)
    target_compile_options(loserfish PRIVATE -march=native)
    target_compile_definitions(loserfish PRIVATE USE_POPCNT)
  endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(loserfish PRIVATE Threads::Threads)
if(LOSERFISH_STATIC)
  target_link_options(loserfish PRIVATE -static)
endif()
