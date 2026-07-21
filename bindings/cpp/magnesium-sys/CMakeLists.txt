cmake_minimum_required(VERSION 3.16)

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
target_compile_options(magnesium-sys PRIVATE -Wall -Wextra -std=c11 -O3)

if(UNIX)
    target_link_libraries(magnesium-sys PUBLIC m dl pthread)
endif()
