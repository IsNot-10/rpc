cmake_minimum_required(VERSION 3.0)
project(gateway)

set(CMAKE_CXX_STANDARD 17)
add_compile_options(-g -Wall)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../http
    ${CMAKE_CURRENT_SOURCE_DIR}/../rpc/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../rpc/example
    ${CMAKE_CURRENT_SOURCE_DIR}/../chat/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/base
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/timer
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/logger
)

# Source files
aux_source_directory(. SRC_LIST)

# HttpServer sources
set(HTTP_SRC 
    ../http/HttpServer.cc 
    ../http/HttpContext.cc 
    ../http/HttpResponse.cc 
    ../http/HttpRequest.cc
    ../http/Router.cc
    ../http/RouteTable.cc
)

# RPC Proto sources
set(PROTO_SRC ../rpc/example/user.pb.cc)

add_executable(GatewayServer ${SRC_LIST} ${HTTP_SRC} ${PROTO_SRC})

target_link_libraries(GatewayServer
    mymuduo
    mprpc
    protobuf pthread
)
