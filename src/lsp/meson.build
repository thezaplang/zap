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
    'workspace.cpp',
    '../sema/binder.cpp',
    '../sema/binder_calls.cpp',
    '../sema/binder_conversions.cpp',
    '../sema/binder_declaration_binding.cpp',
    '../sema/binder_module_predeclare.cpp',
    '../sema/binder_mutability.cpp',
    '../sema/constant_evaluator.cpp',
    '../sema/binder_stmts.cpp',
    '../sema/binder_exprs.cpp',
    '../sema/binder_generic_functions.cpp',
    '../sema/binder_generic_types.cpp',
    '../sema/binder_types.cpp',
    '../utils/stream.cpp'
]

zap_lsp_args = [
    '-DZAPC_CORE_DIR="' + meson.project_source_root() / 'core' + '"',
    '-DZAPC_STDLIB_DIR="' + meson.project_source_root() / 'std' + '"'
]

zap_lsp = executable('zap-lsp',
                     lsp_sources,
                     dependencies: [
                         zap_type_system_dep,
                         zap_sema_conversions_dep,
                         zap_type_layout_dep,
                         zap_frontend_dep,
                         zap_syntax_dep
                     ],
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
                                          '../utils/stream.cpp',
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
