add_subdirectory(callee)
add_subdirectory(caller)

add_executable(test_metrics_unit test_metrics_unit.cc)
target_link_libraries(test_metrics_unit mprpc)
