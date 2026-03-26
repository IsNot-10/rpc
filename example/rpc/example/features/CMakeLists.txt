cmake_minimum_required(VERSION 3.0)
project(features_demo)

set(CMAKE_CXX_STANDARD 17)

# 设置路径
set(RPC_ROOT ${CMAKE_SOURCE_DIR}/../..)
set(MUDUO_ROOT ${RPC_ROOT}/../..)

# RPC Include Paths
include_directories(${RPC_ROOT}/src)
include_directories(${RPC_ROOT}/src/include)
include_directories(${RPC_ROOT}/src/ha)
include_directories(${RPC_ROOT}/src/lb)
include_directories(${RPC_ROOT}/src/metrics)
include_directories(${RPC_ROOT}/src/tracing)
include_directories(${RPC_ROOT}/src/http)

# Muduo Include Paths
include_directories(${MUDUO_ROOT}/src/base)
include_directories(${MUDUO_ROOT}/src/net)
include_directories(${MUDUO_ROOT}/src/timer)
include_directories(${MUDUO_ROOT}/src/logger)

# Link Directories
link_directories(${RPC_ROOT}/lib)
link_directories(${MUDUO_ROOT}/lib)

# Protobuf 生成
find_package(Protobuf REQUIRED)

execute_process(
    COMMAND protoc --cpp_out=${CMAKE_CURRENT_SOURCE_DIR} -I=${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/echo.proto
)

# Server
add_executable(server server.cc echo.pb.cc)
target_link_libraries(server mprpc mymuduo protobuf pthread zookeeper_mt)

# Client
add_executable(client client.cc echo.pb.cc)
target_link_libraries(client mprpc mymuduo protobuf pthread zookeeper_mt)
