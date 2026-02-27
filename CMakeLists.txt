cmake_minimum_required(VERSION 3.20)
project(zapc)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ścieżki do LLVM w Termux
set(LLVM_INCLUDE_DIR "/data/data/com.termux/files/usr/include")
set(LLVM_LIBRARY "/data/data/com.termux/files/usr/lib/libLLVM.so")

# Source files
set(SOURCES
    src/main.cpp
    src/lexer/lexer.cpp
    src/parser/parser.cpp
    src/ir/ir_generator.cpp
    src/sema/binder.cpp
    src/codegen/llvm_codegen.cpp
)

add_executable(zapc ${SOURCES})

target_include_directories(zapc PRIVATE
    ${LLVM_INCLUDE_DIR}
    "${CMAKE_SOURCE_DIR}/src"
)

target_compile_options(zapc PRIVATE -Wall -Wextra)

target_link_libraries(zapc PRIVATE ${LLVM_LIBRARY})

add_custom_command(TARGET zapc POST_BUILD
    COMMAND ${CMAKE_C_COMPILER} -c ${CMAKE_SOURCE_DIR}/src/stdlib.c -o ${CMAKE_BINARY_DIR}/stdlib.o
    COMMENT "Compiling stdlib.c"
)

include(cmake/doxygen.cmake OPTIONAL)
