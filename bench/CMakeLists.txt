add_executable(matchbox_bench bench_main.cpp)

target_link_libraries(matchbox_bench PRIVATE matchbox_core)
target_compile_options(matchbox_bench PRIVATE -Wall -Wextra -O3)
target_compile_definitions(matchbox_bench
  PRIVATE MATCHBOX_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
