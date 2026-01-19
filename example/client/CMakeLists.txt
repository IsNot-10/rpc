
add_executable(client main.cc EchoClient.cc)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/example/client)

target_link_libraries(client mymuduo)