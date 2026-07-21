cmake_minimum_required(VERSION 3.16)

add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE magnesium)
add_test(NAME test_integration COMMAND test_integration)
