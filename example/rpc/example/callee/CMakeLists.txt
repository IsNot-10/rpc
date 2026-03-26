set(SRC_LIST server.cc ../user.pb.cc)

add_executable(server ${SRC_LIST})
target_link_libraries(server mprpc)

