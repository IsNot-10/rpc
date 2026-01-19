cmake_minimum_required(VERSION 3.0)
project(chat)

# 配置编译选项
set(CXX_FLAGS -g -Wall -std=c++17)

# 配置最终的可执行文件输出的路径
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)


# 配置头文件的搜索路径
include_directories(
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/server
    ${PROJECT_SOURCE_DIR}/src/server/dbobj
    ${PROJECT_SOURCE_DIR}/src/server/model
    ${PROJECT_SOURCE_DIR}/src/server/mysql
    ${PROJECT_SOURCE_DIR}/src/server/redis
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/base
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/logger
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/timer
    )

# 加载子目录
add_subdirectory(src)