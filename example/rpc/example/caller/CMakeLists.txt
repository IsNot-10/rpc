set(SRC_LIST client.cc ../user.pb.cc)

# add_executable(client ${SRC_LIST})

# target_link_libraries(client mprpc)


set(STRESS_SRC_LIST StressClient.cc ../user.pb.cc)
add_executable(stress_client ${STRESS_SRC_LIST})
target_link_libraries(stress_client mprpc)

# Verification Client
set(VERIFICATION_SRC_LIST VerificationClient.cc ../user.pb.cc)
add_executable(verification_client ${VERIFICATION_SRC_LIST})
target_link_libraries(verification_client mprpc)
