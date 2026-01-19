# 指定使用CMake的最低版本号
cmake_minimum_required(VERSION 3.0)

# 设置项目名称
project(mprpc)

# 添加搜索路径
include_directories(
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/http
    ${PROJECT_SOURCE_DIR}/example
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/base
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/timer
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/logger
    )



# 包含这些目录下的.cc文件
aux_source_directory(${PROJECT_SOURCE_DIR}/src SRC_RPC)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/http SRC_HTTP)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/lb SRC_LB)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/ha SRC_HA)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/metrics SRC_METRICS)
aux_source_directory(${PROJECT_SOURCE_DIR}/src/tracing SRC_TRACING)



# 设置编译选项
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-g -Wall -O3)



# 链接搜索路径（确保能找到 libmymuduo.so）
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../lib)

# 生成动态库mprpc
add_library(mprpc SHARED ${SRC_RPC} ${SRC_HTTP} ${SRC_LB} ${SRC_HA} ${SRC_METRICS} ${SRC_TRACING} ${SRC_ASYNC})


#目标动态库所需连接的库,除了最最基础的pthread,还有zookeeper_mt
#(zookeeper客户端多线程版本),protobuf(这个项目中最重要的东西,主要反射
#生成类并且提供序列化反序列化操作)以及之前写好的网络库mymuduo
target_link_libraries(
    mprpc 
    mymuduo
    protobuf pthread 
)



# 设置项目可执行文件输出的路径
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)

# 设置生成动态库的路径，放在根目录的lib文件夹下面
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)

# 加载example子目录
add_subdirectory(example)

