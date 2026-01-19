cmake_minimum_required(VERSION 3.0)
project(registry_server)

set(CMAKE_CXX_STANDARD 17)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/base
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/timer
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/logger
)

add_executable(registry_server main.cc RegistryServer.cc)

target_link_libraries(
    registry_server
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/libmymuduo.so
    pthread
)
