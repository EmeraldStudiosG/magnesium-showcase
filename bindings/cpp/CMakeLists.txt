cmake_minimum_required(VERSION 3.16)
project(magnesium-bindings VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(magnesium-sys)
add_subdirectory(magnesium)

enable_testing()
add_subdirectory(tests)
