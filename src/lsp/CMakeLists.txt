set(LSP_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/lsp-main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/lsp.cpp"
  "${CMAKE_SOURCE_DIR}/src/lexer/lexer.cpp"
  "${CMAKE_SOURCE_DIR}/src/parser/parser.cpp"
  "${CMAKE_SOURCE_DIR}/src/sema/binder.cpp"
  "${CMAKE_SOURCE_DIR}/src/utils/stream.cpp"
)

add_executable(zap-lsp ${LSP_SOURCES})

target_include_directories(zap-lsp PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(zap-lsp PRIVATE -Wall -Wextra)
target_compile_definitions(zap-lsp PRIVATE ZAPC_STDLIB_DIR="${CMAKE_SOURCE_DIR}/std")

set_target_properties(zap-lsp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
