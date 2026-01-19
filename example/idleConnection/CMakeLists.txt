
add_executable(EchoServer main.cc EchoServer.cc)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/example/idleConnection)

target_link_libraries(EchoServer mymuduo)
