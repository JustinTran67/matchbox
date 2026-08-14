# Pure logic: partition ownership and the wire format. No Kafka dependency, so the tests
# can cover the ownership property without a broker anywhere in sight.
add_library(matchbox_wire
  partition_assignment.cpp
  wire.cpp
)
target_link_libraries(matchbox_wire PUBLIC matchbox_core nlohmann_json::nlohmann_json)
target_include_directories(matchbox_wire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(matchbox_wire PRIVATE -Wall -Wextra -Wpedantic)

if(NOT MATCHBOX_BUILD_SERVICE)
  return()
endif()

add_library(matchbox_engine_service
  engine_service.cpp
  health_server.cpp
)
target_link_libraries(matchbox_engine_service
  PUBLIC matchbox_wire PkgConfig::RDKAFKA Threads::Threads)
target_compile_options(matchbox_engine_service PRIVATE -Wall -Wextra)

add_executable(matchbox_engine main.cpp)
target_link_libraries(matchbox_engine PRIVATE matchbox_engine_service)
target_compile_options(matchbox_engine PRIVATE -Wall -Wextra)

add_executable(matchbox_e2e e2e_main.cpp)
target_link_libraries(matchbox_e2e PRIVATE matchbox_engine_service)
target_compile_options(matchbox_e2e PRIVATE -Wall -Wextra)
