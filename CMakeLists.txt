cmake_minimum_required(VERSION 3.20)

project(matchbox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

# ThreadSanitizer must instrument every target in the build, so this has to be set before
# any of them are declared. Kept as a separate build directory rather than a default.
option(MATCHBOX_TSAN "Build with ThreadSanitizer" OFF)
if(MATCHBOX_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()

add_library(matchbox_core
  src/build_info.cpp
  src/orderbook/price_level.cpp
  src/orderbook/order_book.cpp
  src/matching/fifo_matcher.cpp
  src/matching/pro_rata_matcher.cpp
  src/matching/engine.cpp
  src/generator/order_generator.cpp
  src/generator/informed_trader.cpp
  src/generator/market_simulator.cpp
)

target_include_directories(matchbox_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(matchbox_core PRIVATE -Wall -Wextra -Wpedantic)

# Optimised builds define NDEBUG, which would strip the engine's internal precondition
# checks out of the very builds the tests run against. Benchmark builds should configure
# with -DMATCHBOX_ENABLE_ASSERTS=OFF so timings are not distorted by them.
option(MATCHBOX_ENABLE_ASSERTS "Keep assertions active in optimised builds" ON)
if(MATCHBOX_ENABLE_ASSERTS)
  target_compile_options(matchbox_core PRIVATE -UNDEBUG)
endif()

enable_testing()
add_subdirectory(sim)
add_subdirectory(tests)
add_subdirectory(bench)
