cmake_minimum_required(VERSION 3.31.6)
project(Ignis)

set(CMAKE_CXX_STANDARD 17)

add_executable(Ignis main.cpp
        frontend/lexer/lexer.h
        frontend/token/token.h
        frontend/lexer/lexer.cpp
        utils/utils.h
        frontend/parser/parser.h
        frontend/ast/node.h
        frontend/parser/parser.cpp
        frontend/ast/arena.h
        frontend/ast/arena.h
        frontend/sema/sema.h
        frontend/sema/sema.cpp
        )
