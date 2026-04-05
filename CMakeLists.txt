cmake_minimum_required(VERSION 3.20)
project(zapc)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use exactly version 21.1.8 as found
find_package(LLVM 21.1.8 REQUIRED CONFIG)

# Source files
set(SOURCES
    src/main.cpp
    src/lexer/lexer.cpp
    src/parser/parser.cpp
    src/ir/ir_generator.cpp
    src/sema/binder.cpp
    src/codegen/llvm_codegen.cpp
    src/driver/driver.cpp
    src/utils/stream.cpp
)

add_executable(zapc ${SOURCES})

target_include_directories(zapc PRIVATE "${CMAKE_SOURCE_DIR}/src" ${LLVM_INCLUDE_DIRS})
target_compile_options(zapc PRIVATE -Wall -Wextra)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/stdlib.o
    COMMAND ${CMAKE_C_COMPILER} -c ${CMAKE_SOURCE_DIR}/src/stdlib.c -o ${CMAKE_BINARY_DIR}/stdlib.o
    DEPENDS ${CMAKE_SOURCE_DIR}/src/stdlib.c
    COMMENT "Compiling stdlib.c"
)

add_custom_target(zap_stdlib ALL DEPENDS ${CMAKE_BINARY_DIR}/stdlib.o)
add_dependencies(zapc zap_stdlib)

# Needed LLVM components
set(LLVM_COMPONENTS
    core
    support
    native
    nativecodegen
    mc
    target
    analysis
    passes
)

# Some distros (e.g. Arch Linux) provide llvm as one shared library rather than multiple ones.
if(LLVM_LINK_LLVM_DYLIB)
    # Single
    set(llvm_libs LLVM)
else()
    # Multiple
    llvm_map_components_to_libnames(llvm_libs ${LLVM_COMPONENTS})
endif()

target_link_libraries(zapc PRIVATE ${llvm_libs})
target_compile_definitions(zapc PRIVATE ${LLVM_DEFINITIONS})
target_compile_definitions(zapc PRIVATE ZAPC_STDLIB_PATH="${CMAKE_BINARY_DIR}/stdlib.o")
target_compile_definitions(zapc PRIVATE ZAPC_STDLIB_DIR="${CMAKE_SOURCE_DIR}/std")

include(cmake/doxygen.cmake)

option(INCLUDE_LSP "Should the LSP binary be compiled" ON)
if(INCLUDE_LSP)
    add_subdirectory(src/lsp)
endif()
