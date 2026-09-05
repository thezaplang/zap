lsp_sources = [
    'lsp-main.cpp',
    'request_dispatcher.cpp',
    'completion.cpp',
    'document_request.cpp',
    'position_codec.cpp',
    'protocol_codec.cpp',
    'language_features.cpp',
    'lsp.cpp',
    'protocol_messages.cpp',
    'protocol_utils.cpp',
    'source_manager.cpp',
    'symbol_index.cpp',
    'workspace.cpp'
]

zap_lsp_args = [
    '-DZAPC_CORE_DIR="' + meson.project_source_root() / 'core' + '"',
    '-DZAPC_STDLIB_DIR="' + meson.project_source_root() / 'std' + '"'
]

zap_lsp = executable('zap-lsp',
                     lsp_sources,
                     dependencies: libzap_dep,
                     cpp_args: zap_lsp_args
)

if get_option('build_testing')
    test('lsp-position', executable('zap-lsp-position-tests',
                                    meson.project_source_root() / 'tests/cpp/lsp_position_test.cpp',
                                    'position_codec.cpp',
                                    include_directories: inc
         ))

    test('lsp-source-manager', executable('zap-lsp-source-manager-tests',
                                          meson.project_source_root() / 'tests/cpp/lsp_source_manager_test.cpp',
                                          'source_manager.cpp',
                                          'protocol_utils.cpp',
                                          'lsp.cpp',
                                          dependencies: zap_support_dep,
                                          include_directories: inc
         ))

    py3 = import('python').find_installation('python3', required: false)
    if py3.found()
        test('lsp-protocol', py3,
             args: [meson.project_source_root() / 'tests/lsp/run_lsp_tests.py'],
             env: {'ZAP_LSP_SERVER': zap_lsp.full_path()},
             depends: zap_lsp
        )
    endif
endif
