set(SRC_LIST client.cc ../user.pb.cc)

add_executable(client ${SRC_LIST})

target_link_libraries(client mprpc)

# Verification Client
add_executable(verification_client VerificationClient.cc ../user.pb.cc)
target_link_libraries(verification_client mprpc)

# Stress Client
add_executable(stress_client StressClient.cc ../user.pb.cc)
target_link_libraries(stress_client mprpc)
