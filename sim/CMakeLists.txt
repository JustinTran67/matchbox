add_library(matchbox_sim symbol_run.cpp)
target_link_libraries(matchbox_sim PUBLIC matchbox_core)
target_include_directories(matchbox_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(matchbox_sim PRIVATE -Wall -Wextra -Wpedantic)

add_executable(matchbox_sim_driver sim_main.cpp)
target_link_libraries(matchbox_sim_driver PRIVATE matchbox_sim Threads::Threads)
target_compile_options(matchbox_sim_driver PRIVATE -Wall -Wextra)
