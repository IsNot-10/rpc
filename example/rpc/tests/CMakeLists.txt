include_directories(../src)
link_directories(../lib)

add_executable(test_metrics_unit test_metrics_unit.cc)
target_link_libraries(test_metrics_unit mprpc)

add_executable(test_lalb_unit lalb_test.cc)
target_link_libraries(test_lalb_unit mprpc pthread)

add_executable(test_lalb_full lalb_full_test.cc)
target_link_libraries(test_lalb_full mprpc pthread)

add_executable(test_consistent_hash consistent_hash_test.cc)
target_link_libraries(test_consistent_hash mprpc pthread)

add_executable(test_circuit_breaker circuit_breaker_test.cc)
target_link_libraries(test_circuit_breaker mprpc pthread)

add_executable(test_tracing tracing_test.cc)
target_link_libraries(test_tracing mprpc pthread)

add_executable(test_metrics_enhanced metrics_test.cc)
target_link_libraries(test_metrics_enhanced mprpc pthread)
