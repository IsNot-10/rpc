
# 定义了一个SRC_LIST变量，包含了该目录下所有的源文件
aux_source_directory(. SRC_LIST)
aux_source_directory(./dbobj DBOBJ_LIST)
aux_source_directory(./model MODEL_LIST)
aux_source_directory(./mysql MYSQL_LIST)
aux_source_directory(./redis REDIS_LIST)

# 指定生成可执行文件
add_executable(
    ChatServer 
    ${SRC_LIST} 
    ${DBOBJ_LIST} 
    ${MODEL_LIST} 
    ${MYSQL_LIST}
    ${REDIS_LIST})

# 指定可执行文件链接时需要依赖的库文件
target_link_libraries(
    ChatServer 
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../lib/libmymuduo.so 
    hiredis mysqlclient pthread)
