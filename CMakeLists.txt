cmake_minimum_required(VERSION 3.31.6)
#cmake_minimum_required(VERSION 3.28.3) # The only version that WSL seems to be able to get at the moment
project(zapc)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")

# Find LLVM
find_package(LLVM REQUIRED CONFIG)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${LLVM_INCLUDE_DIRS})

# Source files
set(SOURCES
    src/main.cpp
    src/lexer/lexer.cpp
    src/parser/parser.cpp
    src/ir/ir_generator.cpp
    src/sema/binder.cpp
    src/codegen/llvm_codegen.cpp
)


set(HEADERS
    src/lexer/lexer.hpp
    src/parser/parser.hpp
    src/parser/ast_builder.hpp
    src/token/token.hpp
    src/ast/node.hpp
    src/ast/nodes.hpp
    src/ast/top_level.hpp
    src/ast/root_node.hpp
    src/ast/statement_node.hpp
    src/ast/expr_node.hpp
    src/ast/body_node.hpp
    src/ast/assign_node.hpp
    src/ast/bin_expr.hpp
    src/ast/unary_expr.hpp
    src/ast/fun_call.hpp
    src/ast/fun_decl.hpp
    src/ast/parameter_node.hpp
    src/ast/type_node.hpp
    src/ast/if_node.hpp
    src/ast/while_node.hpp
    src/ast/break_node.hpp
    src/ast/continue_node.hpp
    src/ast/return_node.hpp
    src/ast/var_decl.hpp
    src/ast/import_node.hpp
    src/ast/const/const_float.hpp
    src/ast/const/const_id.hpp
    src/ast/const/const_int.hpp
    src/ast/const/const_string.hpp
)

# Create executable
add_executable(zapc ${SOURCES} ${HEADERS})

# Custom command to compile stdlib.c
add_custom_command(TARGET zapc POST_BUILD
    COMMAND gcc -c ${CMAKE_SOURCE_DIR}/src/stdlib.c -o ${CMAKE_BINARY_DIR}/stdlib.o
    COMMENT "Compiling stdlib.c"
)

# Link against LLVM libraries
llvm_map_components_to_libnames(llvm_libs core support nativecodegen mc target passes)
target_link_libraries(zapc PRIVATE ${llvm_libs})
    # 

include(cmake/doxygen.cmake)
