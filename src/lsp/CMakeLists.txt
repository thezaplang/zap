set(LSP_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/lsp-main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/lsp.cpp"
)

add_executable(zap-lsp ${LSP_SOURCES})

target_include_directories(zap-lsp PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(zap-lsp PRIVATE -Wall -Wextra)

set_target_properties(zap-lsp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)