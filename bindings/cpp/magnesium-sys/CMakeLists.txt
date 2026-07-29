cmake_minimum_required(VERSION 3.16)

if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR
        "The Magnesium VM uses computed-goto dispatch and cannot be compiled "
        "with cl.exe. Configure this project with Clang or GCC.")
endif()

if(_GNU_SOURCE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions(_GNU_SOURCE)
endif()

set(MG_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

set(MG_SOURCES
    ${MG_REPO_ROOT}/src/lexer.c
    ${MG_REPO_ROOT}/src/parser.c
    ${MG_REPO_ROOT}/src/compiler.c
    ${MG_REPO_ROOT}/src/typecheck.c
    ${MG_REPO_ROOT}/src/object.c
    ${MG_REPO_ROOT}/src/gc.c
    ${MG_REPO_ROOT}/src/vm.c
    ${MG_REPO_ROOT}/src/serialize.c
    ${MG_REPO_ROOT}/src/lsp.c
    ${MG_REPO_ROOT}/src/mg_ffi_glue.c
)

add_library(magnesium-sys STATIC ${MG_SOURCES})
target_include_directories(magnesium-sys PUBLIC
    ${MG_REPO_ROOT}/src
)
target_compile_features(magnesium-sys PRIVATE c_std_11)
if(MSVC)
    target_compile_options(magnesium-sys PRIVATE /W4 /O2)
else()
    target_compile_options(magnesium-sys PRIVATE -Wall -Wextra -O3)
endif()

if(WIN32)
    target_link_libraries(magnesium-sys PUBLIC ws2_32)
elseif(UNIX)
    target_link_libraries(magnesium-sys PUBLIC m pthread)
    if(NOT APPLE)
        target_link_libraries(magnesium-sys PUBLIC dl)
    endif()
endif()
