cmake_minimum_required(VERSION 3.20) # 3.2 should be compatible with this CMakeLists.txt
#cmake_minimum_required(VERSION 3.28.3) # The only version that WSL seems to be able to get at the moment
project(zapc)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find LLVM
find_package(LLVM REQUIRED CONFIG)

# Source files
set(SOURCES
    src/main.cpp
    src/lexer/lexer.cpp
    src/parser/parser.cpp
    src/ir/ir_generator.cpp
    src/sema/binder.cpp
    src/codegen/llvm_codegen.cpp
)

# Create executable
add_executable(zapc ${SOURCES})

# Include directories
target_include_directories(zapc PRIVATE "${CMAKE_SOURCE_DIR}/src")

# Compiler options
target_compile_options(zapc PRIVATE -Wall -Wextra)

# Compile stdlib.c
add_library(zap_stdlib OBJECT src/stdlib.c)

# Needed LLVM components if its split
set(LLVM_COMPONENTS
    core
    support
    nativecodegen
    mc
    target
    passes
)

# Find LLVM, this should work assuming LLVM is installed system-wide
find_package(LLVM REQUIRED CONFIG)

# Check if LLVM is one big library or split ones
if(LLVM_LINK_LLVM_DYLIB)
    # Single
    set(llvm_libs LLVM)
else()
    # Multiple
    llvm_map_components_to_libnames(llvm_libs ${LLVM_COMPONENTS})
endif()

# Link LLVM to zapc
target_link_libraries(zapc PRIVATE ${llvm_libs})

# LLVM often requires specific compiler definitions 
target_compile_definitions(zapc PRIVATE ${LLVM_DEFINITIONS})

include(cmake/doxygen.cmake)
