cmake_minimum_required(VERSION 3.16)
project(lsmkv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LSMKV_BUILD_TESTS "Build the test suite" ON)
option(LSMKV_BUILD_BENCH "Build the benchmark driver" ON)
option(LSMKV_BUILD_TOOLS "Build the command line tool" ON)
option(LSMKV_ASAN "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LSMKV_TSAN "Build with ThreadSanitizer" OFF)
option(LSMKV_WERROR "Treat warnings as errors" OFF)

add_library(lsmkv
  src/util/crc32c.cpp
  src/util/bloom.cpp
  src/util/env.cpp
  src/memtable/memtable.cpp
  src/wal/log.cpp
  src/table/block.cpp
  src/table/iterators.cpp
  src/table/sstable.cpp
  src/table/table_cache.cpp
  src/db/dbformat.cpp
  src/db/version.cpp
  src/db/write_batch.cpp
  src/db/db_impl.cpp
)

target_include_directories(lsmkv
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

find_package(Threads REQUIRED)
target_link_libraries(lsmkv PUBLIC Threads::Threads)

if(MSVC)
  target_compile_options(lsmkv PRIVATE /W4)
else()
  target_compile_options(lsmkv PRIVATE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
    -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
  )
endif()

if(LSMKV_WERROR AND NOT MSVC)
  target_compile_options(lsmkv PRIVATE -Werror)
endif()

# The sanitizers are mutually exclusive: ASan and TSan cannot be linked into
# the same binary.
if(LSMKV_ASAN AND LSMKV_TSAN)
  message(FATAL_ERROR "LSMKV_ASAN and LSMKV_TSAN cannot both be enabled")
endif()

set(LSMKV_SAN_FLAGS "")
if(LSMKV_ASAN)
  set(LSMKV_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
elseif(LSMKV_TSAN)
  set(LSMKV_SAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
endif()

if(LSMKV_SAN_FLAGS)
  target_compile_options(lsmkv PUBLIC ${LSMKV_SAN_FLAGS})
  target_link_options(lsmkv PUBLIC ${LSMKV_SAN_FLAGS})
endif()

# A helper so every executable gets the private include path and sanitizers.
function(lsmkv_executable name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE lsmkv)
  target_include_directories(${name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  if(NOT MSVC)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
  endif()
endfunction()

if(LSMKV_BUILD_TESTS)
  enable_testing()
  lsmkv_executable(lsmkv_tests
    tests/main.cpp
    tests/util_test.cpp
    tests/memtable_test.cpp
    tests/log_test.cpp
    tests/sstable_test.cpp
    tests/db_test.cpp
    tests/recovery_test.cpp
  )
  add_test(NAME unit COMMAND lsmkv_tests)
  # The randomized and crash tests are slower; CI runs them as their own case
  # so a failure says which half broke.
  set_tests_properties(unit PROPERTIES TIMEOUT 900)
endif()

if(LSMKV_BUILD_BENCH)
  lsmkv_executable(lsmkv_bench bench/bench.cpp)
endif()

if(LSMKV_BUILD_TOOLS)
  lsmkv_executable(lsmkv_cli tools/cli.cpp)
endif()
