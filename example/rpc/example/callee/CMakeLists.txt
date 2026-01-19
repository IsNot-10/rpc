set(SRC_LIST server.cc ../user.pb.cc)
set(HELLO_SRC_LIST HelloServer.cc ../Hello.pb.cc)

add_executable(server ${SRC_LIST})
target_link_libraries(server mprpc)

add_executable(hello_server ${HELLO_SRC_LIST})
target_link_libraries(hello_server mprpc)
