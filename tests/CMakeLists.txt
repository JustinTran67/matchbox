include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.8.1
  GIT_SHALLOW ON
)
FetchContent_MakeAvailable(Catch2)

add_executable(matchbox_tests
  test_order_book.cpp
  test_matching.cpp
  test_stress.cpp
  test_pro_rata.cpp
  test_multi_symbol.cpp
  test_partitioning.cpp
)

target_link_libraries(matchbox_tests PRIVATE matchbox_sim matchbox_wire Threads::Threads Catch2::Catch2WithMain)
target_include_directories(matchbox_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(matchbox_tests PRIVATE -Wall -Wextra)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(matchbox_tests)
