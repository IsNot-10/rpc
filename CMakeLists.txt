# 指定使用CMake的最低版本号
cmake_minimum_required(VERSION 3.0)

# 设置项目名称
project(mymuduo)

# 添加搜索路径
include_directories(
    ${PROJECT_SOURCE_DIR}/src/base
    ${PROJECT_SOURCE_DIR}/src/net
    ${PROJECT_SOURCE_DIR}/src/timer
    ${PROJECT_SOURCE_DIR}/src/logger
    )

# 包含这些目录下的.cc文件
aux_source_directory(${PROJECT_SOURCE_DIR}/src/base SRC_BASE)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/net SRC_NET)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/timer SRC_TIMER)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/logger SRC_LOGGER)

# 设置编译选项
set(CMAKE_CXX_FLAGS "-O3 -Wall -std=c++17")

# 生成动态库mymuduo
add_library(mymuduo SHARED 
            ${SRC_BASE}
            ${SRC_NET}
            ${SRC_TIMER}
            ${SRC_LOGGER}
            )

# 目标动态库所需连接的库(这里需要连接libpthread.so)
target_link_libraries(mymuduo pthread)

# 设置生成动态库的路径，放在根目录的lib文件夹下面
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)


# idleConnection模块测试,这是关于实现了时间轮的EchoServer,算是最基础的测试
add_subdirectory(example/idleConnection)


# TcpClient模块测试,muduo客户端的测试
#add_subdirectory(example/client)


# http模块测试,实现了解析get请求的webserver
add_subdirectory(example/http)

# RPC 框架
add_subdirectory(example/rpc)

# 注册中心
add_subdirectory(example/registry)

# Gateway 网关
add_subdirectory(example/gateway)