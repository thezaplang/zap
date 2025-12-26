cmake_minimum_required(VERSION 3.31.6)
project(Zap)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")

# Include directories
include_directories(${CMAKE_SOURCE_DIR})

# Source files
set(SOURCES
    main.cpp
    lexer/lexer.cpp
    parser/parser.cpp
)


set(HEADERS
    lexer/lexer.hpp
    parser/parser.hpp
    token/token.hpp
    ast/node.hpp
    ast/nodes.hpp
    ast/top_level.hpp
    ast/root_node.hpp
    ast/statement_node.hpp
    ast/expr_node.hpp
    ast/body_node.hpp
    ast/assign_node.hpp
    ast/bin_expr.hpp
    ast/unary_expr.hpp
    ast/fun_call.hpp
    ast/fun_decl.hpp
    ast/parameter_node.hpp
    ast/type_node.hpp
    ast/if_node.hpp
    ast/while_node.hpp
    ast/break_node.hpp
    ast/continue_node.hpp
    ast/return_node.hpp
    ast/var_decl.hpp
    ast/import_node.hpp
    ast/const/const_float.hpp
    ast/const/const_id.hpp
    ast/const/const_int.hpp
    ast/const/const_string.hpp
)

# Create executable
add_executable(Zap ${SOURCES} ${HEADERS})

# Link against standard libraries if needed
target_link_libraries(Zap PRIVATE)
