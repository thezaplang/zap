cmake_minimum_required(VERSION 3.20) # 3.2 should be compatible with this CMakeLists.txt
#cmake_minimum_required(VERSION 3.28.3) # The only version that WSL seems to be able to get at the moment
project(zapc)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find LLVM
find_package(LLVM REQUIRED CONFIG)

# Add tablegen
find_program(LLVM_TBLGEN_EXE NAMES llvm-tblgen REQUIRED)
# Set the option tablegen file
set(OPT_TD "${CMAKE_CURRENT_SOURCE_DIR}/src/driver/Options.td")
# Set the tablegen output
set(OPT_INC "${CMAKE_CURRENT_SOURCE_DIR}/src/driver/Options.inc")

# Call tablegen
add_custom_command(
    OUTPUT ${OPT_INC}
    COMMAND ${LLVM_TBLGEN_EXE} -gen-opt-parser-defs 
            -I ${LLVM_INCLUDE_DIRS} 
            ${OPT_TD} -o ${OPT_INC}
    DEPENDS ${OPT_TD}
    COMMENT "Generating Options.inc from Options.td"
)

add_custom_target(ZapcOptGen DEPENDS ${OPT_INC})

# Source files
set(SOURCES
    src/main.cpp
    src/lexer/lexer.cpp
    src/parser/parser.cpp
    src/ir/ir_generator.cpp
    src/sema/binder.cpp
    src/codegen/llvm_codegen.cpp
    src/driver/driver.cpp
)

# Create executable
add_executable(zapc ${SOURCES})

# Tablegen dependency
add_dependencies(zapc ZapcOptGen)

# Include directories
target_include_directories(zapc PRIVATE "${CMAKE_SOURCE_DIR}/src")

# Compiler options
target_compile_options(zapc PRIVATE -Wall -Wextra)

# Compile stdlib.c
add_custom_command(TARGET zapc POST_BUILD
    COMMAND ${CMAKE_C_COMPILER} -c ${CMAKE_SOURCE_DIR}/src/stdlib.c -o ${CMAKE_BINARY_DIR}/stdlib.o
    COMMENT "Compiling stdlib.c"
)

# Needed LLVM components if its split
set(LLVM_COMPONENTS
    core
    support
    nativecodegen
    mc
    target
    passes
    option
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

# Add a stdlib.o path macro
target_compile_definitions(zapc PRIVATE ZAPC_STDLIB_PATH="${CMAKE_BINARY_DIR}/stdlib.o")

include(cmake/doxygen.cmake)
