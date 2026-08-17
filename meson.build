project('zapc', ['c', 'cpp'],
        version : '0.4.0',
        default_options : [
            'cpp_std=c++17',
            'warning_level=2',
        ]
)

cpp = meson.get_compiler('cpp')
c = meson.get_compiler('c')

if cpp.get_id() in ['gcc', 'clang']
    add_project_arguments('-Werror=switch', language : 'cpp')
endif

if get_option('zap_enable_sanitizers')
    if cpp.get_id() not in ['gcc', 'clang']
        error('ZAP_ENABLE_SANITIZERS requires a GNU or Clang compiler')
    endif
    san_args = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    add_project_arguments(san_args, language : ['c', 'cpp'])
    add_project_link_arguments('-fsanitize=address,undefined', language : ['c', 'cpp'])
endif

zap_stdlib_compile_defs = []
if get_option('zap_enable_runtime_instrumentation')
    zap_stdlib_compile_defs += '-DZAP_RUNTIME_INSTRUMENTATION=1'
    add_project_arguments('-DZAP_RUNTIME_INSTRUMENTATION=1', language : ['c', 'cpp'])
endif

llvm_dep = dependency('llvm',
                      version : '>= 21',
                      modules : ['core', 'support', 'all-targets', 'mc', 'target', 'analysis', 'passes'],
                      required : true
)
openssl_dep = dependency('openssl', required : true)
m_dep = c.find_library('m', required : false)

inc = include_directories('src')
tomlc17_inc = include_directories('third_party/tomlc17/src')

tomlc17 = static_library('tomlc17',
                         'third_party/tomlc17/src/tomlc17.c',
                         include_directories : tomlc17_inc)
tomlc17_dep = declare_dependency(link_with : tomlc17,
                                 include_directories : tomlc17_inc)

zap_type_system = static_library('zap_type_system',
                                 'src/ir/failable_type.cpp',
                                 'src/ir/primitive_type.cpp',
                                 'src/ir/type_identity.cpp',
                                 include_directories : inc
)
zap_type_system_dep = declare_dependency(link_with : zap_type_system, include_directories : inc)

zap_sema_conversions = static_library('zap_sema_conversions',
                                      'src/sema/conversion.cpp',
                                      dependencies : zap_type_system_dep,
                                      include_directories : inc
)
zap_sema_conversions_dep = declare_dependency(link_with : zap_sema_conversions, include_directories : inc)

zap_type_layout = static_library('zap_type_layout',
                                 'src/sema/type_layout.cpp',
                                 dependencies : zap_type_system_dep,
                                 include_directories : inc
)
zap_type_layout_dep = declare_dependency(link_with : zap_type_layout, include_directories : inc)

zap_syntax = static_library('zap_syntax',
                            'src/lexer/lexer.cpp',
                            'src/parser/parser.cpp',
                            'src/parser/parser_declarations.cpp',
                            include_directories : inc
)
zap_syntax_dep = declare_dependency(link_with : zap_syntax, include_directories : inc)

zap_frontend = static_library('zap_frontend',
                              'src/frontend/frontend_session.cpp',
                              'src/frontend/module_loader.cpp',
                              'src/frontend/project_configuration.cpp',
                              dependencies : [zap_syntax_dep, tomlc17_dep],
                              include_directories : inc
)
zap_frontend_dep = declare_dependency(link_with : zap_frontend, include_directories : inc)

zap_zir_verifier = static_library('zap_zir_verifier',
                                  'src/ir/borrow_provenance.cpp',
                                  'src/ir/borrow_verifier.cpp',
                                  'src/ir/call_contract.cpp',
                                  'src/ir/control_flow_graph.cpp',
                                  'src/ir/dead_phi_elimination.cpp',
                                  'src/ir/ownership_flow.cpp',
                                  'src/ir/ownership_lowering.cpp',
                                  'src/ir/ownership_liveness.cpp',
                                  'src/ir/zir_verifier.cpp',
                                  'src/ir/zir_verifier_function.cpp',
                                  'src/ir/zir_verifier_types.cpp',
                                  dependencies : zap_type_system_dep,
                                  include_directories : inc
)
zap_zir_verifier_dep = declare_dependency(link_with : zap_zir_verifier, include_directories : inc)

stdlib_o = custom_target('zap_stdlib',
                         input : 'src/stdlib.c',
                         output : 'stdlib.o',
                         command : [c.cmd_array(), zap_stdlib_compile_defs, '-c', '@INPUT@', '-o', '@OUTPUT@'],
                         depend_files : [
                             'src/runtime/arc_layout.h',
                             'src/runtime/string_layout.h'
                         ],
                         build_by_default : true
)

zapc_sources = [
    'src/main.cpp', 'src/ir/ir_generator.cpp', 'src/ir/function_reachability.cpp', 'src/sema/binder.cpp',
    'src/sema/binder_calls.cpp', 'src/sema/binder_conversions.cpp',
    'src/sema/binder_declaration_binding.cpp', 'src/sema/binder_module_predeclare.cpp',
    'src/sema/binder_stmts.cpp', 'src/sema/binder_exprs.cpp',
    'src/sema/binder_mutability.cpp',
    'src/sema/constant_evaluator.cpp',
    'src/sema/binder_generic_functions.cpp', 'src/sema/binder_generic_types.cpp',
    'src/sema/binder_types.cpp', 'src/codegen/llvm_codegen.cpp',
    'src/codegen/llvm_codegen_constants.cpp', 'src/codegen/llvm_codegen_zir.cpp',
    'src/codegen/llvm_codegen_arc.cpp', 'src/codegen/class_arc_emitter.cpp',
    'src/driver/driver.cpp', 'src/driver/process.cpp', 'src/driver/args/argparse.cpp',
    'src/utils/stream.cpp'
]

zapc_args = [
    '-DZAPC_STDLIB_PATH="' + stdlib_o.full_path() + '"',
    '-DZAPC_CORE_DIR="' + meson.current_source_dir() / 'core' + '"',
    '-DZAPC_STDLIB_DIR="' + meson.current_source_dir() / 'std' + '"'
]

zapc = executable('zapc',
                  zapc_sources,
                  dependencies : [
                      llvm_dep,
                      zap_type_system_dep,
                      zap_sema_conversions_dep,
                      zap_type_layout_dep,
                      zap_frontend_dep,
                      zap_syntax_dep,
                      zap_zir_verifier_dep
                  ],
                  cpp_args : zapc_args,
)

doxygen = find_program('doxygen', required : false)
if doxygen.found()
    run_target('doxygen', command : [doxygen, meson.current_source_dir() / 'Doxyfile'])
endif

if get_option('build_testing')
    test('type-identity', executable('zap-type-identity-tests', 'tests/cpp/type_identity_test.cpp', dependencies : zap_type_system_dep))
    test('conversions', executable('zap-conversion-tests', 'tests/cpp/conversion_test.cpp', dependencies : zap_sema_conversions_dep))
    test('type-layout', executable('zap-type-layout-tests', 'tests/cpp/type_layout_test.cpp', dependencies : zap_type_layout_dep))
    test('zir-verifier', executable('zap-zir-verifier-tests', 'tests/cpp/zir_verifier_test.cpp', dependencies : zap_zir_verifier_dep))
    test('ownership-lowering', executable('zap-ownership-lowering-tests', 'tests/cpp/ownership_lowering_test.cpp', dependencies : zap_zir_verifier_dep))
    test('borrow-analysis', executable('zap-borrow-analysis-tests', 'tests/cpp/borrow_analysis_test.cpp', dependencies : zap_zir_verifier_dep))
    test('project-configuration', executable('zap-project-configuration-tests',
                                              'tests/cpp/project_configuration_test.cpp',
                                              dependencies : zap_frontend_dep))
    test('function-reachability', executable('zap-function-reachability-tests', ['tests/cpp/function_reachability_test.cpp', 'src/ir/function_reachability.cpp'], dependencies : zap_type_system_dep))
    test('zir-reachability',
         files('tests/scripts/check_zir_reachability.sh'),
         args : [
             zapc.full_path(),
             meson.current_source_dir() / 'tests/zir_reachability/main.zp',
             meson.current_source_dir() / 'tests/zir_reachability/caller.c',
             meson.current_source_dir() / 'tests/zir_reachability/library.zp',
             meson.current_build_dir() / 'zir-reachability-test'
         ],
         depends : zapc
    )
    test('zir-class-reachability',
         files('tests/scripts/check_zir_class_reachability.sh'),
         args : [
             zapc.full_path(),
             meson.current_source_dir() / 'tests/zir_class_reachability/main.zp',
             meson.current_build_dir() / 'zir-class-reachability-test.zir'
         ],
         depends : zapc
    )

    runtime_test = executable('zap-runtime-instrumentation-tests',
                              'tests/cpp/runtime_instrumentation_test.c', 'src/stdlib.c',
                              include_directories : inc,
                              c_args : ['-DZAP_RUNTIME_INSTRUMENTATION=1'],
                              dependencies : [m_dep, openssl_dep]
    )
    test('runtime-instrumentation', runtime_test)

    if get_option('zap_enable_runtime_instrumentation')
        test('runtime-instrumentation-ir',
             files('tests/scripts/check_runtime_instrumentation_ir.sh'),
             args : [
                 zapc.full_path(),
                 meson.current_source_dir() / 'tests/class_arc_test.zp',
                 meson.current_build_dir() / 'runtime-instrumentation.ll'
             ],
             depends : zapc
        )

        test('runtime-ownership-counts',
             files('tests/scripts/run_runtime_ownership_counts.sh'),
             args : [
                 zapc.full_path(),
                 meson.current_source_dir() / 'tests/runtime_instrumentation/ownership_counts.zp',
                 meson.current_build_dir() / 'runtime-ownership-counts-test'
             ],
             depends : zapc
        )
    endif
endif

if get_option('include_lsp')
    subdir('src/lsp')
endif
