cmake_minimum_required(VERSION 3.16)

add_library(magnesium INTERFACE)
target_include_directories(magnesium INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(magnesium INTERFACE magnesium-sys)
