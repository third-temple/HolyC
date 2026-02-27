cmake_minimum_required(VERSION 3.21)

project(
  holyc
  VERSION 0.1.0
  DESCRIPTION "Cross-platform HolyC compiler"
  LANGUAGES C CXX
)

include(CTest)

option(HOLYC_ENABLE_LLVM "Enable LLVM integration when LLVM is available" ON)
option(HOLYC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

function(holyc_detect_host_profile out_var)
  string(TOLOWER "${CMAKE_HOST_SYSTEM_NAME}" _holyc_host_os)
  string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _holyc_host_arch)
  if(_holyc_host_arch STREQUAL "amd64")
    set(_holyc_host_arch "x86_64")
  elseif(_holyc_host_arch STREQUAL "aarch64")
    set(_holyc_host_arch "arm64")
  endif()
  set(${out_var} "${_holyc_host_os}-${_holyc_host_arch}" PARENT_SCOPE)
endfunction()

holyc_detect_host_profile(HOLYC_BUNDLED_LLVM_PROFILE_DEFAULT)
set(
  HOLYC_BUNDLED_LLVM_PROFILE
  "${HOLYC_BUNDLED_LLVM_PROFILE_DEFAULT}"
  CACHE STRING
  "Bundled LLVM build profile tag"
)
set(
  HOLYC_BUNDLED_LLVM_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llvm/llvm"
  CACHE PATH
  "Bundled LLVM source directory"
)
set(
  HOLYC_BUNDLED_LLVM_BUILD_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llvm/build-${HOLYC_BUNDLED_LLVM_PROFILE}"
  CACHE PATH
  "Bundled LLVM build directory"
)
set(
  HOLYC_BUNDLED_LLVM_CONFIG_DIR
  "${HOLYC_BUNDLED_LLVM_BUILD_DIR}/lib/cmake/llvm"
  CACHE PATH
  "Path to bundled LLVMConfig.cmake directory"
)
set(
  HOLYC_BUNDLED_LLVM_BUILD_JOBS
  "2"
  CACHE STRING
  "Max parallel jobs for bundled LLVM bootstrap build"
)
if(NOT HOLYC_BUNDLED_LLVM_BUILD_JOBS MATCHES "^[1-9][0-9]*$")
  message(WARNING
    "Invalid HOLYC_BUNDLED_LLVM_BUILD_JOBS='${HOLYC_BUNDLED_LLVM_BUILD_JOBS}', using 2")
  set(HOLYC_BUNDLED_LLVM_BUILD_JOBS "2")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(HolycCompilerOptions)

holyc_configure_global_compiler_options()

set(HOLYC_LLVM_ENABLED FALSE)
if(NOT HOLYC_ENABLE_LLVM)
  message(FATAL_ERROR "HOLYC_ENABLE_LLVM=OFF is no longer supported. LLVM is required.")
endif()

if(NOT EXISTS "${HOLYC_BUNDLED_LLVM_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "Bundled LLVM source tree is missing at ${HOLYC_BUNDLED_LLVM_SOURCE_DIR}. "
    "Run: git submodule update --init --recursive third_party/llvm")
endif()

set(_holyc_llvm_config "${HOLYC_BUNDLED_LLVM_CONFIG_DIR}/LLVMConfig.cmake")
if(NOT EXISTS "${_holyc_llvm_config}")
  message(STATUS "Bundled LLVM config not found at ${_holyc_llvm_config}; bootstrapping bundled LLVM build")
  file(MAKE_DIRECTORY "${HOLYC_BUNDLED_LLVM_BUILD_DIR}")

  set(_holyc_llvm_build_type "Release")
  if(NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE)
    set(_holyc_llvm_build_type "${CMAKE_BUILD_TYPE}")
  endif()

  set(_holyc_llvm_targets "AArch64\\;X86")
  set(
    _holyc_llvm_configure_cmd
    "${CMAKE_COMMAND}"
    -S "${HOLYC_BUNDLED_LLVM_SOURCE_DIR}"
    -B "${HOLYC_BUNDLED_LLVM_BUILD_DIR}"
    "-DCMAKE_BUILD_TYPE=${_holyc_llvm_build_type}"
    "-DLLVM_PARALLEL_COMPILE_JOBS=${HOLYC_BUNDLED_LLVM_BUILD_JOBS}"
    -DLLVM_PARALLEL_LINK_JOBS=1
    -DLLVM_ENABLE_PROJECTS=
    "-DLLVM_TARGETS_TO_BUILD=${_holyc_llvm_targets}"
    -DLLVM_INCLUDE_TESTS=OFF
    -DLLVM_INCLUDE_EXAMPLES=OFF
    -DLLVM_INCLUDE_BENCHMARKS=OFF
    -DLLVM_BUILD_TOOLS=OFF
    -DLLVM_ENABLE_BINDINGS=OFF
    -DLLVM_ENABLE_ZSTD=OFF
    -DBUILD_SHARED_LIBS=OFF
    -DLLVM_BUILD_LLVM_DYLIB=OFF
    -DLLVM_LINK_LLVM_DYLIB=OFF
  )
  if(CMAKE_GENERATOR)
    list(INSERT _holyc_llvm_configure_cmd 1 -G "${CMAKE_GENERATOR}")
  endif()
  if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND _holyc_llvm_configure_cmd -A "${CMAKE_GENERATOR_PLATFORM}")
  endif()
  if(CMAKE_GENERATOR_TOOLSET)
    list(APPEND _holyc_llvm_configure_cmd -T "${CMAKE_GENERATOR_TOOLSET}")
  endif()
  if(DEFINED CMAKE_OSX_ARCHITECTURES AND NOT "${CMAKE_OSX_ARCHITECTURES}" STREQUAL "")
    list(APPEND _holyc_llvm_configure_cmd "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
  endif()
  if(DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT "${CMAKE_OSX_DEPLOYMENT_TARGET}" STREQUAL "")
    list(APPEND _holyc_llvm_configure_cmd "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  endif()
  if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND _holyc_llvm_configure_cmd "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
  endif()

  execute_process(
    COMMAND ${_holyc_llvm_configure_cmd}
    RESULT_VARIABLE _holyc_llvm_configure_result
  )
  if(NOT _holyc_llvm_configure_result EQUAL 0)
    message(FATAL_ERROR
      "Failed to configure bundled LLVM in ${HOLYC_BUNDLED_LLVM_BUILD_DIR} "
      "(exit code ${_holyc_llvm_configure_result})")
  endif()

  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" --build "${HOLYC_BUNDLED_LLVM_BUILD_DIR}"
      --parallel "${HOLYC_BUNDLED_LLVM_BUILD_JOBS}"
    RESULT_VARIABLE _holyc_llvm_build_result
  )
  if(NOT _holyc_llvm_build_result EQUAL 0)
    message(FATAL_ERROR
      "Failed to build bundled LLVM in ${HOLYC_BUNDLED_LLVM_BUILD_DIR} "
      "(exit code ${_holyc_llvm_build_result})")
  endif()

  if(NOT EXISTS "${_holyc_llvm_config}")
    message(FATAL_ERROR
      "Bundled LLVM finished building but ${_holyc_llvm_config} is still missing")
  endif()
endif()

find_package(LLVM CONFIG REQUIRED PATHS "${HOLYC_BUNDLED_LLVM_CONFIG_DIR}" NO_DEFAULT_PATH)

message(STATUS "Using bundled LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
set(HOLYC_LLVM_ENABLED TRUE)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
  @ONLY
)

add_executable(holyc
  frontend/diagnostics.cpp
  frontend/frontend.cpp
  frontend/hir.cpp
  frontend/parser.cpp
  frontend/preprocessor.cpp
  frontend/sema.cpp
  lowering/llvm_backend.cpp
  lowering/llvm_irbuilder_backend.cpp
  runtime/hc_runtime.cpp
  src/main.cpp
)

target_include_directories(
  holyc
  PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
  "${CMAKE_CURRENT_SOURCE_DIR}/frontend"
  "${CMAKE_CURRENT_SOURCE_DIR}/lowering"
  "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
)
target_compile_definitions(holyc PRIVATE HOLYC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(holyc PRIVATE cxx_std_20)

holyc_apply_target_warnings(holyc)

if(HOLYC_WARNINGS_AS_ERRORS)
  holyc_target_warnings_as_errors(holyc)
endif()

if(HOLYC_LLVM_ENABLED)
  target_compile_definitions(holyc PRIVATE HOLYC_HAS_LLVM=1)

  set(HOLYC_LLVM_LIBS "")
  if(DEFINED LLVM_AVAILABLE_LIBS)
    set(HOLYC_LLVM_LIBS ${LLVM_AVAILABLE_LIBS})
    list(REMOVE_ITEM HOLYC_LLVM_LIBS LLVMLineEditor)
  else()
    llvm_map_components_to_libnames(
      HOLYC_LLVM_LIBS
      core
      support
      irreader
      target
      executionengine
      runtimedyld
      orcjit
      nativecodegen
      nativeasmparser
      nativeasmprinter
    )
  endif()

  set(HOLYC_LLVM_INCLUDE_DIRS "")
  if(DEFINED LLVM_INCLUDE_DIRS)
    list(APPEND HOLYC_LLVM_INCLUDE_DIRS ${LLVM_INCLUDE_DIRS})
  endif()
  if(DEFINED LLVM_INCLUDE_DIR)
    list(APPEND HOLYC_LLVM_INCLUDE_DIRS ${LLVM_INCLUDE_DIR})
  endif()
  if(HOLYC_LLVM_INCLUDE_DIRS STREQUAL "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llvm/llvm/include")
      list(APPEND HOLYC_LLVM_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llvm/llvm/include"
      )
    endif()
    if(EXISTS "${HOLYC_BUNDLED_LLVM_BUILD_DIR}/include")
      list(APPEND HOLYC_LLVM_INCLUDE_DIRS
        "${HOLYC_BUNDLED_LLVM_BUILD_DIR}/include"
      )
    endif()
  endif()
  list(REMOVE_DUPLICATES HOLYC_LLVM_INCLUDE_DIRS)
  if(HOLYC_LLVM_INCLUDE_DIRS)
    target_include_directories(holyc SYSTEM PRIVATE ${HOLYC_LLVM_INCLUDE_DIRS})
  endif()
  set(HOLYC_LLVM_COMPILE_DEFINITIONS "")
  set(HOLYC_LLVM_COMPILE_OPTIONS "")
  set(HOLYC_LLVM_DEFINITION_TOKENS "")
  foreach(_holyc_llvm_def_raw IN LISTS LLVM_DEFINITIONS)
    if(_holyc_llvm_def_raw STREQUAL "")
      continue()
    endif()
    set(_holyc_llvm_split_tokens "")
    separate_arguments(_holyc_llvm_split_tokens UNIX_COMMAND "${_holyc_llvm_def_raw}")
    if(_holyc_llvm_split_tokens)
      list(APPEND HOLYC_LLVM_DEFINITION_TOKENS ${_holyc_llvm_split_tokens})
    else()
      list(APPEND HOLYC_LLVM_DEFINITION_TOKENS "${_holyc_llvm_def_raw}")
    endif()
  endforeach()
  foreach(_holyc_llvm_def IN LISTS HOLYC_LLVM_DEFINITION_TOKENS)
    if(_holyc_llvm_def MATCHES "^-D(.+)")
      list(APPEND HOLYC_LLVM_COMPILE_DEFINITIONS "${CMAKE_MATCH_1}")
    elseif(_holyc_llvm_def MATCHES "^-")
      list(APPEND HOLYC_LLVM_COMPILE_OPTIONS "${_holyc_llvm_def}")
    else()
      list(APPEND HOLYC_LLVM_COMPILE_DEFINITIONS "${_holyc_llvm_def}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES HOLYC_LLVM_COMPILE_DEFINITIONS)
  list(REMOVE_DUPLICATES HOLYC_LLVM_COMPILE_OPTIONS)
  if(HOLYC_LLVM_COMPILE_DEFINITIONS)
    target_compile_definitions(holyc PRIVATE ${HOLYC_LLVM_COMPILE_DEFINITIONS})
  endif()
  if(HOLYC_LLVM_COMPILE_OPTIONS)
    target_compile_options(holyc PRIVATE ${HOLYC_LLVM_COMPILE_OPTIONS})
  endif()
  target_link_libraries(holyc PRIVATE ${HOLYC_LLVM_LIBS} ${LLVM_SYSTEM_LIBS})
  if(APPLE)
    # LLVM static targets carry transitive link interfaces that intentionally
    # repeat some archives; suppress noisy macOS ld duplicate-library warnings.
    target_link_options(holyc PRIVATE "LINKER:-no_warn_duplicate_libraries")
  endif()
endif()

install(TARGETS holyc RUNTIME DESTINATION bin)

if(BUILD_TESTING)
  add_executable(runtime_abi_conformance
    runtime/hc_runtime.cpp
    tests/runtime/runtime_abi_conformance.cpp
  )
  target_include_directories(runtime_abi_conformance PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
  target_compile_features(runtime_abi_conformance PRIVATE cxx_std_20)
  holyc_apply_target_warnings(runtime_abi_conformance)
  if(HOLYC_WARNINGS_AS_ERRORS)
    holyc_target_warnings_as_errors(runtime_abi_conformance)
  endif()

  add_test(NAME runtime.abi.conformance COMMAND $<TARGET_FILE:runtime_abi_conformance>)

  if(HOLYC_LLVM_ENABLED)
    add_executable(jit_backend_conformance
      lowering/llvm_backend.cpp
      runtime/hc_runtime.cpp
      tests/runtime/jit_backend_conformance.cpp
    )
    target_include_directories(
      jit_backend_conformance
      PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/lowering"
      "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    )
    target_compile_definitions(
      jit_backend_conformance
      PRIVATE
      HOLYC_HAS_LLVM=1
      HOLYC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if(HOLYC_LLVM_COMPILE_DEFINITIONS)
      target_compile_definitions(jit_backend_conformance PRIVATE ${HOLYC_LLVM_COMPILE_DEFINITIONS})
    endif()
    if(HOLYC_LLVM_COMPILE_OPTIONS)
      target_compile_options(jit_backend_conformance PRIVATE ${HOLYC_LLVM_COMPILE_OPTIONS})
    endif()
    if(HOLYC_LLVM_INCLUDE_DIRS)
      target_include_directories(jit_backend_conformance SYSTEM PRIVATE ${HOLYC_LLVM_INCLUDE_DIRS})
    endif()
    target_link_libraries(jit_backend_conformance PRIVATE ${HOLYC_LLVM_LIBS} ${LLVM_SYSTEM_LIBS})
    if(APPLE)
      target_link_options(jit_backend_conformance PRIVATE "LINKER:-no_warn_duplicate_libraries")
    endif()
    target_compile_features(jit_backend_conformance PRIVATE cxx_std_20)
    holyc_apply_target_warnings(jit_backend_conformance)
    if(HOLYC_WARNINGS_AS_ERRORS)
      holyc_target_warnings_as_errors(jit_backend_conformance)
    endif()

    add_test(NAME runtime.jit_backend.conformance COMMAND $<TARGET_FILE:jit_backend_conformance>)
  endif()

  add_test(NAME holyc.version COMMAND $<TARGET_FILE:holyc> --version)
  add_test(NAME holyc.strict-default COMMAND $<TARGET_FILE:holyc> --print-strict-mode)
  add_test(
    NAME holyc.check.features
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/features.HC"
  )
  set_tests_properties(holyc.check.features PROPERTIES PASS_REGULAR_EXPRESSION "ok")
  add_test(
    NAME holyc.check.permissive
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/features.HC" --permissive
  )
  set_tests_properties(holyc.check.permissive PROPERTIES PASS_REGULAR_EXPRESSION "ok")
  add_test(
    NAME holyc.check.strict-decl-modifiers
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/decl_modifiers.HC"
  )
  set_tests_properties(holyc.check.strict-decl-modifiers PROPERTIES WILL_FAIL TRUE)
  add_test(
    NAME holyc.check.permissive-decl-modifiers
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/decl_modifiers.HC" --permissive
  )
  set_tests_properties(holyc.check.permissive-decl-modifiers PROPERTIES PASS_REGULAR_EXPRESSION "ok")
  add_test(
    NAME holyc.check.syntax-error
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_missing_semi.HC"
  )
  set_tests_properties(holyc.check.syntax-error PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.check.ref-exceptions
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/holyc_docs/Exceptions.HC"
  )
  set_tests_properties(holyc.check.ref-exceptions PROPERTIES PASS_REGULAR_EXPRESSION "ok")

  add_test(
    NAME holyc.check.ref-lastclass
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/holyc_docs/LastClass.HC"
  )
  set_tests_properties(holyc.check.ref-lastclass PROPERTIES PASS_REGULAR_EXPRESSION "ok")

  add_test(
    NAME holyc.check.ref-lock
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/holyc_docs/Lock.HC"
  )
  set_tests_properties(holyc.check.ref-lock PROPERTIES PASS_REGULAR_EXPRESSION "ok")

  add_test(
    NAME holyc.check.ref-stkgrow
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/holyc_docs/StkGrow.HC"
  )
  set_tests_properties(holyc.check.ref-stkgrow PROPERTIES PASS_REGULAR_EXPRESSION "ok")

  add_test(
    NAME holyc.check.ref-classmeta
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/holyc_docs/ClassMeta.HC"
  )
  set_tests_properties(holyc.check.ref-classmeta PROPERTIES PASS_REGULAR_EXPRESSION "ok")

  add_test(
    NAME holyc.check.main-with-params
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/main_with_params.HC"
  )
  set_tests_properties(holyc.check.main-with-params PROPERTIES PASS_REGULAR_EXPRESSION "ok")

  add_test(
    NAME holyc.check.print-star-width-precision
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/print_star_width_precision.HC"
  )
  set_tests_properties(holyc.check.print-star-width-precision PROPERTIES PASS_REGULAR_EXPRESSION "ok")
  add_test(
    NAME holyc.check.print-float-formats
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/print_float_formats.HC"
  )
  set_tests_properties(holyc.check.print-float-formats PROPERTIES PASS_REGULAR_EXPRESSION "ok")

  add_test(
    NAME holyc.check.anon-aggregate-trailing
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/anon_aggregate_trailing_decl.HC"
  )
  set_tests_properties(holyc.check.anon-aggregate-trailing PROPERTIES PASS_REGULAR_EXPRESSION "ok")

  add_test(
    NAME holyc.check.indirect-call-forms
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/indirect_call_forms.HC"
  )
  set_tests_properties(holyc.check.indirect-call-forms PROPERTIES PASS_REGULAR_EXPRESSION "ok")

  add_test(
    NAME holyc.check.inline-asm-multi-output
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/inline_asm_multi_output.HC"
  )
  set_tests_properties(holyc.check.inline-asm-multi-output PROPERTIES PASS_REGULAR_EXPRESSION "ok")

  add_test(
    NAME holyc.check.lane-assignments
    COMMAND $<TARGET_FILE:holyc> check "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/lane_assignments.HC"
  )
  set_tests_properties(holyc.check.lane-assignments PROPERTIES PASS_REGULAR_EXPRESSION "ok")
  add_test(
    NAME holyc.ast-dump.features
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/features.HC"
  )

  add_test(
    NAME holyc.emit-hir.jit
    COMMAND $<TARGET_FILE:holyc> emit-hir "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/jit.HC"
  )
  set_tests_properties(holyc.emit-hir.jit PROPERTIES PASS_REGULAR_EXPRESSION "Function: Main")

  add_test(
    NAME holyc.emit-hir.print-stmt-formats
    COMMAND $<TARGET_FILE:holyc> emit-hir "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/print_stmt_formats.HC"
  )
  set_tests_properties(holyc.emit-hir.print-stmt-formats PROPERTIES PASS_REGULAR_EXPRESSION "Print: \"%d %s %c %d")

  add_test(
    NAME holyc.emit-hir.class-trailing-decls
    COMMAND $<TARGET_FILE:holyc> emit-hir "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/class_trailing_decls.HC"
  )
  set_tests_properties(holyc.emit-hir.class-trailing-decls PROPERTIES PASS_REGULAR_EXPRESSION "Function: Main")

  add_test(
    NAME holyc.emit-hir.subint-lane-access
    COMMAND $<TARGET_FILE:holyc> emit-hir "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/subint_lane_access.HC"
  )
  set_tests_properties(holyc.emit-hir.subint-lane-access PROPERTIES PASS_REGULAR_EXPRESSION "Expr\\(lane\\): i16")

  add_test(
    NAME holyc.emit-hir.lastclass-default
    COMMAND $<TARGET_FILE:holyc> emit-hir "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/lastclass_default.HC"
  )
  set_tests_properties(holyc.emit-hir.lastclass-default PROPERTIES PASS_REGULAR_EXPRESSION "Expr\\(string-literal\\): \"Student\"")

  add_test(
    NAME holyc.emit-hir.lock-stmt
    COMMAND $<TARGET_FILE:holyc> emit-hir "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/lock_stmt.HC"
  )
  set_tests_properties(holyc.emit-hir.lock-stmt PROPERTIES PASS_REGULAR_EXPRESSION "Lock")

  add_test(
    NAME holyc.emit-hir.default-arg-expr
    COMMAND $<TARGET_FILE:holyc> emit-hir "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/default_arg_expr.HC"
  )
  set_tests_properties(holyc.emit-hir.default-arg-expr PROPERTIES PASS_REGULAR_EXPRESSION "Function: Main")

  add_test(
    NAME holyc.ast-dump.syntax-error
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_missing_semi.HC"
  )
  set_tests_properties(holyc.ast-dump.syntax-error PROPERTIES WILL_FAIL TRUE)
  add_test(
    NAME holyc.ast-dump.invalid-unterminated-block-comment
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_unterminated_block_comment.HC"
  )
  set_tests_properties(
    holyc.ast-dump.invalid-unterminated-block-comment
    PROPERTIES
      WILL_FAIL TRUE
  )

  add_test(
    NAME holyc.ast-dump.semantics
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/semantics.HC"
  )

  add_test(
    NAME holyc.ast-dump.invalid-continue
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_continue.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-continue PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-missing-required-arg
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_missing_required_arg.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-missing-required-arg PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.labels-goto
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/labels_goto.HC"
  )

  add_test(
    NAME holyc.ast-dump.decl-grammar
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/decl_grammar.HC"
  )

  add_test(
    NAME holyc.ast-dump.type-grammar
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/type_grammar.HC"
  )

  add_test(
    NAME holyc.ast-dump.expr-grammar
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/expr_grammar.HC"
  )

  add_test(
    NAME holyc.ast-dump.label-scope-grammar
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/label_scope_grammar.HC"
  )

  add_test(
    NAME holyc.ast-dump.inline-asm-parse
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/inline_asm_parse.HC"
  )

  add_test(
    NAME holyc.ast-dump.linkage-forms
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/linkage_forms.HC" --permissive
  )

  add_test(
    NAME holyc.ast-dump.metadata-parse
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/metadata_parse.HC"
  )

  add_test(
    NAME holyc.ast-dump.symbol-table
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/symbol_table.HC"
  )

  add_test(
    NAME holyc.ast-dump.type-conversions
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/type_conversions.HC"
  )

  add_test(
    NAME holyc.ast-dump.int-promotion
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/int_promotion.HC"
  )

  add_test(
    NAME holyc.ast-dump.pointer-arith
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/pointer_arith.HC"
  )

  add_test(
    NAME holyc.ast-dump.linkage-validation-ok
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/linkage_validation_ok.HC"
  )

  add_test(
    NAME holyc.ast-dump.goto-over-uninitialized
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/goto_over_uninitialized_decl.HC"
  )

  add_test(
    NAME holyc.ast-dump.class-member-typing
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/class_member_typing.HC"
  )

  add_test(
    NAME holyc.ast-dump.union-member-typing
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/union_member_typing.HC"
  )

  add_test(
    NAME holyc.ast-dump.exception-throw-across-call
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/exception_throw_across_call.HC"
  )

  add_test(
    NAME holyc.ast-dump.comma-expr-lowering
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_comma_expr_lowering.HC"
  )

  add_test(
    NAME holyc.ast-dump.expression-space-lowering
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/expression_space_lowering.HC"
  )

  add_test(
    NAME holyc.ast-dump.static-local-decl
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/static_local_decl.HC"
  )

  add_test(
    NAME holyc.ast-dump.multi-vardecl
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/multi_vardecl.HC"
  )

  add_test(
    NAME holyc.ast-dump.class-trailing-decls
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/class_trailing_decls.HC"
  )

  add_test(
    NAME holyc.ast-dump.print-stmt-formats
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/print_stmt_formats.HC"
  )

  add_test(
    NAME holyc.ast-dump.adjacent-string-literals
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/adjacent_string_literals.HC"
  )

  add_test(
    NAME holyc.ast-dump.dollar-expr
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/dollar_expr.HC"
  )

  add_test(
    NAME holyc.ast-dump.postfix-cast
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/postfix_cast.HC"
  )

  add_test(
    NAME holyc.ast-dump.subint-lane-access
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/subint_lane_access.HC"
  )

  add_test(
    NAME holyc.ast-dump.lastclass-default
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/lastclass_default.HC"
  )

  add_test(
    NAME holyc.ast-dump.decl-modifiers
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/decl_modifiers.HC" --permissive
  )

  add_test(
    NAME holyc.ast-dump.static-function-linkage
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/static_function_linkage.HC"
  )

  add_test(
    NAME holyc.ast-dump.extern-global-redeclaration
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/extern_global_redeclaration.HC"
  )

  add_test(
    NAME holyc.ast-dump.constexpr-global-init
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/constexpr_global_init.HC"
  )

  add_test(
    NAME holyc.ast-dump.callstkgrow-runtime
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/callstkgrow_runtime.HC"
  )

  add_test(
    NAME holyc.ast-dump.job-queue-runtime
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/job_queue_runtime.HC"
  )

  add_test(
    NAME holyc.ast-dump.metadata-runtime-apis
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/metadata_runtime_apis.HC"
  )

  add_test(
    NAME holyc.ast-dump.invalid-print-format-args
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_print_format_args.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-print-format-args PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-import-global-conflict
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_import_global_conflict.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-import-global-conflict PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-pointer-float-assign
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_pointer_float_assign.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-pointer-float-assign PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-pointer-add
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_pointer_add.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-pointer-add PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-duplicate-function-def
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/duplicate_function_def.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-duplicate-function-def PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-conflicting-function-decl
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/conflicting_function_decl.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-conflicting-function-decl PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-import-function-body
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/import_function_with_body.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-import-function-body PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-goto-across-init
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_goto_across_init.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-goto-across-init PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-goto-into-scope
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_goto_into_scope.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-goto-into-scope PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-unknown-member
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_unknown_member.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-unknown-member PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-duplicate-field
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_duplicate_field.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-duplicate-field PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-duplicate-union-decl
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_duplicate_union_decl.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-duplicate-union-decl PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-throw-float-payload
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_throw_float_payload.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-throw-float-payload PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-inline-asm-template
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_inline_asm_no_template.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-inline-asm-template PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-inline-asm-operand-without-constraint
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_inline_asm_operand_without_constraint.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-inline-asm-operand-without-constraint PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-inline-asm-missing-operand
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_inline_asm_missing_operand.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-inline-asm-missing-operand PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.public-modifier
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_public_modifier.HC"
  )
  set_tests_properties(holyc.ast-dump.public-modifier PROPERTIES WILL_FAIL TRUE)
  add_test(
    NAME holyc.ast-dump.public-modifier-permissive
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_public_modifier.HC" --permissive
  )

  add_test(
    NAME holyc.ast-dump.invalid-lane-index
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_lane_index.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-lane-index PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.ast-dump.invalid-lane-base-type
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_lane_base_type.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-lane-base-type PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.emit-llvm.labels-goto-aot
    COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/labels_goto.HC"
  )
  set_tests_properties(
    holyc.emit-llvm.labels-goto-aot
    PROPERTIES
      PASS_REGULAR_EXPRESSION "label\\.Done"
  )

  add_test(
    NAME holyc.emit-llvm.invalid-comma-expr-lowering
    COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_comma_expr_lowering.HC"
  )
  set_tests_properties(
    holyc.emit-llvm.invalid-comma-expr-lowering
    PROPERTIES
      PASS_REGULAR_EXPRESSION "define i64 @Main"
  )

  add_test(
    NAME holyc.emit-llvm.trycatch
    COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/runtime_try.HC"
  )
  set_tests_properties(holyc.emit-llvm.trycatch PROPERTIES PASS_REGULAR_EXPRESSION "_setjmp")

  add_test(
    NAME holyc.emit-llvm.inline-asm
    COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/inline_asm_parse.HC"
  )
  set_tests_properties(holyc.emit-llvm.inline-asm PROPERTIES PASS_REGULAR_EXPRESSION "asm sideeffect")

  add_test(
    NAME holyc.emit-llvm.inline-asm-constraints
    COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_inline_asm_constraints_not_supported.HC"
  )
  set_tests_properties(
    holyc.emit-llvm.inline-asm-constraints
    PROPERTIES
      PASS_REGULAR_EXPRESSION "asm sideeffect"
  )

  add_test(
    NAME holyc.ast-dump.inline-asm-nop
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/inline_asm_nop.HC"
  )
  set_tests_properties(holyc.ast-dump.inline-asm-nop PROPERTIES PASS_REGULAR_EXPRESSION "AsmStmt")

  add_test(
    NAME holyc.ast-dump.main-with-params
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/main_with_params.HC"
  )
  set_tests_properties(holyc.ast-dump.main-with-params PROPERTIES PASS_REGULAR_EXPRESSION "FunctionDecl")

  add_test(
    NAME holyc.ast-dump.print-star-width-precision
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/print_star_width_precision.HC"
  )
  set_tests_properties(holyc.ast-dump.print-star-width-precision PROPERTIES PASS_REGULAR_EXPRESSION "PrintStmt")

  add_test(
    NAME holyc.ast-dump.anon-aggregate-trailing
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/anon_aggregate_trailing_decl.HC"
  )
  set_tests_properties(holyc.ast-dump.anon-aggregate-trailing PROPERTIES PASS_REGULAR_EXPRESSION "ClassDecl")

  add_test(
    NAME holyc.ast-dump.indirect-call-forms
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/indirect_call_forms.HC"
  )
  set_tests_properties(holyc.ast-dump.indirect-call-forms PROPERTIES PASS_REGULAR_EXPRESSION "CallExpr")

  add_test(
    NAME holyc.ast-dump.inline-asm-multi-output
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/inline_asm_multi_output.HC"
  )
  set_tests_properties(holyc.ast-dump.inline-asm-multi-output PROPERTIES PASS_REGULAR_EXPRESSION "AsmStmt")

  add_test(
    NAME holyc.ast-dump.lane-assignments
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/lane_assignments.HC"
  )
  set_tests_properties(holyc.ast-dump.lane-assignments PROPERTIES PASS_REGULAR_EXPRESSION "AssignExpr")

  add_test(
    NAME holyc.ast-dump.inline-asm-input-operand
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/inline_asm_input_operand.HC"
  )
  set_tests_properties(holyc.ast-dump.inline-asm-input-operand PROPERTIES PASS_REGULAR_EXPRESSION "AsmArg")

  if(HOLYC_LLVM_ENABLED)
    add_test(
      NAME holyc.emit-llvm.subint-lane-access
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/subint_lane_access.HC"
    )
    set_tests_properties(holyc.emit-llvm.subint-lane-access PROPERTIES PASS_REGULAR_EXPRESSION "lshr")

    add_test(
      NAME holyc.emit-llvm.inline-asm-nop
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/inline_asm_nop.HC"
    )
    set_tests_properties(holyc.emit-llvm.inline-asm-nop PROPERTIES PASS_REGULAR_EXPRESSION "asm sideeffect")

    add_test(
      NAME holyc.emit-llvm.main-with-params
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/main_with_params.HC"
    )
    set_tests_properties(holyc.emit-llvm.main-with-params PROPERTIES PASS_REGULAR_EXPRESSION "define i32 @main\\(i32 %argc, ptr %argv\\)")

    add_test(
      NAME holyc.emit-llvm.print-star-width-precision
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/print_star_width_precision.HC"
    )
    set_tests_properties(holyc.emit-llvm.print-star-width-precision PROPERTIES PASS_REGULAR_EXPRESSION "call void @hc_print_fmt")

    add_test(
      NAME holyc.emit-llvm.anon-aggregate-trailing
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/anon_aggregate_trailing_decl.HC"
    )
    set_tests_properties(holyc.emit-llvm.anon-aggregate-trailing PROPERTIES PASS_REGULAR_EXPRESSION "@g_pair = global")

    add_test(
      NAME holyc.emit-llvm.indirect-call-forms
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/indirect_call_forms.HC"
    )
    set_tests_properties(holyc.emit-llvm.indirect-call-forms PROPERTIES PASS_REGULAR_EXPRESSION "call i64 %")

    add_test(
      NAME holyc.emit-llvm.inline-asm-multi-output
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/inline_asm_multi_output.HC"
    )
    set_tests_properties(holyc.emit-llvm.inline-asm-multi-output PROPERTIES PASS_REGULAR_EXPRESSION "call \\{ i64, i64 \\} asm sideeffect")

    add_test(
      NAME holyc.emit-llvm.lane-assignments
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/lane_assignments.HC"
    )
    set_tests_properties(holyc.emit-llvm.lane-assignments PROPERTIES PASS_REGULAR_EXPRESSION "shl i64")

    add_test(
      NAME holyc.emit-llvm.inline-asm-input-operand
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/inline_asm_input_operand.HC"
    )
    set_tests_properties(holyc.emit-llvm.inline-asm-input-operand PROPERTIES PASS_REGULAR_EXPRESSION "call void asm sideeffect")
  endif()

  add_test(
    NAME holyc.emit-llvm.metadata-aot
    COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/metadata_parse.HC"
  )
  set_tests_properties(
    holyc.emit-llvm.metadata-aot
    PROPERTIES
      PASS_REGULAR_EXPRESSION "hc_register_reflection_table"
  )

  if(HOLYC_LLVM_ENABLED)
    add_test(
      NAME holyc.emit-llvm.print-runtime-formatting
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/print_runtime_formatting.HC"
    )
    set_tests_properties(holyc.emit-llvm.print-runtime-formatting PROPERTIES PASS_REGULAR_EXPRESSION "hc_print_fmt")
  endif()

  if(HOLYC_LLVM_ENABLED)
    add_test(
      NAME holyc.emit-llvm.expr-grammar-aot
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/expr_grammar.HC"
    )
    set_tests_properties(holyc.emit-llvm.expr-grammar-aot PROPERTIES PASS_REGULAR_EXPRESSION "define i64 @Main")
  endif()

  add_test(
    NAME holyc.emit-llvm.invalid-expression-space-lowering
    COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/expression_space_lowering.HC"
  )
  if(HOLYC_LLVM_ENABLED)
    set_tests_properties(
      holyc.emit-llvm.invalid-expression-space-lowering
      PROPERTIES
        PASS_REGULAR_EXPRESSION "getelementptr.*%hc\\.Pair"
    )
  else()
    set_tests_properties(
      holyc.emit-llvm.invalid-expression-space-lowering
      PROPERTIES
        WILL_FAIL TRUE
        PASS_REGULAR_EXPRESSION "unsupported expression kind: comma"
    )
  endif()

  add_test(
    NAME holyc.jit.labels-goto-jit
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/labels_goto.HC"
  )
  set_tests_properties(
    holyc.jit.labels-goto-jit
    PROPERTIES
      PASS_REGULAR_EXPRESSION "0"
  )

  add_test(
    NAME holyc.jit.invalid-comma-expr-lowering
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_comma_expr_lowering.HC"
  )
  set_tests_properties(
    holyc.jit.invalid-comma-expr-lowering
    PROPERTIES
      PASS_REGULAR_EXPRESSION "3"
  )

  add_test(
    NAME holyc.jit.expression-space-lowering
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/expression_space_lowering.HC"
  )
  set_tests_properties(
    holyc.jit.expression-space-lowering
    PROPERTIES
      PASS_REGULAR_EXPRESSION "1"
  )

  add_test(
    NAME holyc.ast-dump.invalid-goto-unknown-label
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_goto_unknown_label.HC"
  )
  set_tests_properties(holyc.ast-dump.invalid-goto-unknown-label PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.preprocess.basic
    COMMAND $<TARGET_FILE:holyc> preprocess "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess.HC"
  )

  add_test(
    NAME holyc.preprocess.token-aware
    COMMAND $<TARGET_FILE:holyc> preprocess "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_token_aware.HC"
  )
  set_tests_properties(holyc.preprocess.token-aware PROPERTIES PASS_REGULAR_EXPRESSION "\"TOKEN\\\\n\"")

  add_test(
    NAME holyc.preprocess.function-macros
    COMMAND $<TARGET_FILE:holyc> preprocess "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_function_macros.HC"
  )
  set_tests_properties(holyc.preprocess.function-macros PROPERTIES PASS_REGULAR_EXPRESSION "return \\(\\(3\\)\\+\\(1\\)\\);")

  add_test(
    NAME holyc.preprocess.recursive-macro-guard
    COMMAND $<TARGET_FILE:holyc> preprocess "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_recursive_macro_guard.HC"
  )
  set_tests_properties(holyc.preprocess.recursive-macro-guard PROPERTIES PASS_REGULAR_EXPRESSION "SELF;")

  add_test(
    NAME holyc.preprocess.golden.basic.jit
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/preprocess/run_golden_preprocess.sh"
            $<TARGET_FILE:holyc>
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess.HC"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/preprocess/preprocess_jit.golden"
            jit
  )

  add_test(
    NAME holyc.preprocess.golden.preprocess-modes.aot
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/preprocess/run_golden_preprocess.sh"
            $<TARGET_FILE:holyc>
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_modes.HC"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/preprocess/preprocess_aot.golden"
            aot
  )

  add_test(
    NAME holyc.preprocess.golden.function-macros.jit
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/preprocess/run_golden_preprocess.sh"
            $<TARGET_FILE:holyc>
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_function_macros.HC"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/preprocess/function_macros_jit.golden"
            jit
  )

  add_test(
    NAME holyc.preprocess.golden.ref-stkgrow.jit
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/preprocess/run_golden_preprocess.sh"
            $<TARGET_FILE:holyc>
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/ref_stkgrow_preprocess.HC"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/preprocess/ref_stkgrow_jit.golden"
            jit
  )

  add_test(
    NAME holyc.preprocess.golden.ref-lock.jit
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/preprocess/run_golden_preprocess.sh"
            $<TARGET_FILE:holyc>
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/ref_lock_preprocess.HC"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/preprocess/ref_lock_jit.golden"
            jit
  )

  add_test(
    NAME holyc.preprocess.golden.ref-directives.jit
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/preprocess/run_golden_preprocess.sh"
            $<TARGET_FILE:holyc>
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/ref_directives_preprocess.HC"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/preprocess/ref_directives_jit.golden"
            jit
  )

  add_test(
    NAME holyc.hir.golden.jit
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/hir/run_golden_hir.sh"
            $<TARGET_FILE:holyc>
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/jit.HC"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/hir/jit.hir.golden"
  )

  add_test(
    NAME holyc.ast.golden.features
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/ast/run_golden_ast.sh"
            $<TARGET_FILE:holyc>
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/features.HC"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/ast/features.ast.golden"
  )

  add_test(
    NAME holyc.ast-dump.preprocess
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess.HC"
  )

  add_test(
    NAME holyc.preprocess.invalid-exe
    COMMAND $<TARGET_FILE:holyc> preprocess "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_unsupported_exe.HC"
  )
  set_tests_properties(holyc.preprocess.invalid-exe PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.preprocess.invalid-exe-extra-arg
    COMMAND $<TARGET_FILE:holyc> preprocess "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_unsupported_exe_extra_arg.HC"
  )
  set_tests_properties(holyc.preprocess.invalid-exe-extra-arg PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.preprocess.invalid-include-cycle
    COMMAND $<TARGET_FILE:holyc> preprocess "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/include_cycle_a.HC"
  )
  set_tests_properties(holyc.preprocess.invalid-include-cycle PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.emit-llvm.llvm
    COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/llvm.HC"
  )

  if(HOLYC_LLVM_ENABLED)
    add_test(
      NAME holyc.emit-llvm.lock-atomic
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/lock_stmt.HC"
    )
    set_tests_properties(holyc.emit-llvm.lock-atomic PROPERTIES PASS_REGULAR_EXPRESSION "atomicrmw add")

    add_test(
      NAME holyc.emit-llvm.global-initializers
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/global_initializers.HC"
    )
    set_tests_properties(holyc.emit-llvm.global-initializers PROPERTIES PASS_REGULAR_EXPRESSION "@g_msg = global")

    add_test(
      NAME holyc.emit-llvm.member-layout-lowering
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/member_layout_lowering.HC"
    )
    set_tests_properties(holyc.emit-llvm.member-layout-lowering PROPERTIES PASS_REGULAR_EXPRESSION "getelementptr inbounds")

    add_test(
      NAME holyc.emit-llvm.static-function-linkage
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/static_function_linkage.HC"
    )
    set_tests_properties(holyc.emit-llvm.static-function-linkage PROPERTIES PASS_REGULAR_EXPRESSION "define internal i64 @Hidden")

    add_test(
      NAME holyc.emit-llvm.extern-global-redeclaration
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/extern_global_redeclaration.HC"
    )
    set_tests_properties(holyc.emit-llvm.extern-global-redeclaration PROPERTIES PASS_REGULAR_EXPRESSION "@g_count = global i64 7")

    add_test(
      NAME holyc.emit-llvm.constexpr-global-init
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/constexpr_global_init.HC"
    )
    set_tests_properties(holyc.emit-llvm.constexpr-global-init PROPERTIES PASS_REGULAR_EXPRESSION "@b = global i64 8")

    add_test(
      NAME holyc.emit-llvm.callstkgrow-runtime
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/callstkgrow_runtime.HC"
    )
    set_tests_properties(holyc.emit-llvm.callstkgrow-runtime PROPERTIES PASS_REGULAR_EXPRESSION "call i64 @CallStkGrow")

    add_test(
      NAME holyc.emit-llvm.job-queue-runtime
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/job_queue_runtime.HC"
    )
    set_tests_properties(holyc.emit-llvm.job-queue-runtime PROPERTIES PASS_REGULAR_EXPRESSION "call ptr @JobQue")

    add_test(
      NAME holyc.emit-llvm.metadata-runtime-apis
      COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/metadata_runtime_apis.HC"
    )
    set_tests_properties(holyc.emit-llvm.metadata-runtime-apis PROPERTIES PASS_REGULAR_EXPRESSION "call ptr @HashFind")
  endif()

  add_test(
    NAME holyc.emit-llvm.invalid-string-literal
    COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_llvm_string_expr.HC"
  )
  if(HOLYC_LLVM_ENABLED)
    set_tests_properties(holyc.emit-llvm.invalid-string-literal PROPERTIES PASS_REGULAR_EXPRESSION "ret i64")
  else()
    set_tests_properties(holyc.emit-llvm.invalid-string-literal PROPERTIES WILL_FAIL TRUE)
  endif()

  add_test(
    NAME holyc.build.llvm
    COMMAND $<TARGET_FILE:holyc> build "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/llvm.HC" -o "${CMAKE_CURRENT_BINARY_DIR}/llvm_hello"
  )

  add_test(
    NAME holyc.build.llvm.artifacts
    COMMAND $<TARGET_FILE:holyc> build "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/llvm.HC"
            -o "${CMAKE_CURRENT_BINARY_DIR}/llvm_hello_artifacts"
            --artifact-dir="${CMAKE_CURRENT_BINARY_DIR}/artifacts"
            --keep-temps
  )

  add_test(
    NAME holyc.build.invalid-no-main
    COMMAND $<TARGET_FILE:holyc> build "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_no_main_for_build.HC" -o "${CMAKE_CURRENT_BINARY_DIR}/no_main_should_fail"
  )
  set_tests_properties(holyc.build.invalid-no-main PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.jit.jit
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/jit.HC"
  )
  set_tests_properties(holyc.jit.jit PROPERTIES PASS_REGULAR_EXPRESSION "10")

  add_test(
    NAME holyc.jit.invalid-no-main
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_no_main_for_build.HC"
  )
  set_tests_properties(holyc.jit.invalid-no-main PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME holyc.jit.runtime
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/runtime_try.HC"
  )
  set_tests_properties(holyc.jit.runtime PROPERTIES PASS_REGULAR_EXPRESSION "7")

  add_test(
    NAME holyc.jit.switch
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/switch_compat.HC"
  )
  set_tests_properties(holyc.jit.switch PROPERTIES PASS_REGULAR_EXPRESSION "1")

  add_test(
    NAME holyc.emit-llvm.default-args
    COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/default_args_lowering.HC"
  )

  add_test(
    NAME holyc.jit.default-args
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/default_args_lowering.HC"
  )
  set_tests_properties(holyc.jit.default-args PROPERTIES PASS_REGULAR_EXPRESSION "11")

  add_test(
    NAME holyc.build.default-args
    COMMAND $<TARGET_FILE:holyc> build "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/default_args_lowering.HC" -o "${CMAKE_CURRENT_BINARY_DIR}/default_args"
  )

  add_test(
    NAME holyc.jit.control-flow
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/control_flow.HC"
  )
  set_tests_properties(holyc.jit.control-flow PROPERTIES PASS_REGULAR_EXPRESSION "14")

  add_test(
    NAME holyc.build.control-flow
    COMMAND $<TARGET_FILE:holyc> build "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/control_flow.HC" -o "${CMAKE_CURRENT_BINARY_DIR}/control_flow"
  )

  add_test(
    NAME holyc.jit.runtime-print
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/runtime_print.HC"
  )
  set_tests_properties(holyc.jit.runtime-print PROPERTIES PASS_REGULAR_EXPRESSION "3")

  add_test(
    NAME holyc.build.runtime-print
    COMMAND $<TARGET_FILE:holyc> build "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/runtime_print.HC" -o "${CMAKE_CURRENT_BINARY_DIR}/runtime_print"
  )

  add_test(
    NAME holyc.run.runtime-print
    COMMAND $<TARGET_FILE:holyc> run "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/runtime_print.HC"
  )
  set_tests_properties(holyc.run.runtime-print PROPERTIES PASS_REGULAR_EXPRESSION "hello")

  add_test(
    NAME holyc.jit.expr-ops
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/expr_ops.HC"
  )
  set_tests_properties(holyc.jit.expr-ops PROPERTIES PASS_REGULAR_EXPRESSION "15")

  add_test(
    NAME holyc.jit.type-conversions
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/type_conversions.HC"
  )
  set_tests_properties(holyc.jit.type-conversions PROPERTIES PASS_REGULAR_EXPRESSION "1")

  add_test(
    NAME holyc.jit.int-promotion
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/int_promotion.HC"
  )
  set_tests_properties(holyc.jit.int-promotion PROPERTIES PASS_REGULAR_EXPRESSION "6")

  add_test(
    NAME holyc.jit.linkage-validation-ok
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/linkage_validation_ok.HC"
  )
  set_tests_properties(holyc.jit.linkage-validation-ok PROPERTIES PASS_REGULAR_EXPRESSION "2")

  add_test(
    NAME holyc.jit.exception-throw-across-call
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/exception_throw_across_call.HC"
  )
  set_tests_properties(holyc.jit.exception-throw-across-call PROPERTIES PASS_REGULAR_EXPRESSION "11")

  add_test(
    NAME holyc.jit.static-local-decl
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/static_local_decl.HC"
  )
  set_tests_properties(holyc.jit.static-local-decl PROPERTIES PASS_REGULAR_EXPRESSION "3")

  add_test(
    NAME holyc.jit.reflection-runtime
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/reflection_runtime.HC"
  )
  set_tests_properties(holyc.jit.reflection-runtime PROPERTIES PASS_REGULAR_EXPRESSION "2")

  add_test(
    NAME holyc.run.reflection-runtime
    COMMAND $<TARGET_FILE:holyc> run "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/reflection_runtime.HC"
  )
  set_tests_properties(holyc.run.reflection-runtime PROPERTIES PASS_REGULAR_EXPRESSION "2")

  add_test(
    NAME holyc.jit.switch-edge-cases
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/switch_edge_cases.HC"
  )
  set_tests_properties(holyc.jit.switch-edge-cases PROPERTIES PASS_REGULAR_EXPRESSION "30")

  add_test(
    NAME holyc.jit.switch-start-end-edge
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/switch_start_end_edge.HC"
  )
  set_tests_properties(holyc.jit.switch-start-end-edge PROPERTIES PASS_REGULAR_EXPRESSION "4")

  add_test(
    NAME holyc.jit.multi-vardecl
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/multi_vardecl.HC"
  )
  set_tests_properties(holyc.jit.multi-vardecl PROPERTIES PASS_REGULAR_EXPRESSION "10")

  add_test(
    NAME holyc.jit.print-stmt-formats
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/print_stmt_formats.HC"
  )
  set_tests_properties(holyc.jit.print-stmt-formats PROPERTIES PASS_REGULAR_EXPRESSION "T")
  add_test(
    NAME holyc.jit.print-float-formats
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/print_float_formats.HC"
  )
  set_tests_properties(holyc.jit.print-float-formats PROPERTIES PASS_REGULAR_EXPRESSION "7\\.00 7\\.0e\\+00 7")

  add_test(
    NAME holyc.jit.adjacent-string-literals
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/adjacent_string_literals.HC"
  )
  set_tests_properties(holyc.jit.adjacent-string-literals PROPERTIES PASS_REGULAR_EXPRESSION "Hello, HolyC")

  add_test(
    NAME holyc.jit.dollar-expr
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/dollar_expr.HC"
  )
  set_tests_properties(holyc.jit.dollar-expr PROPERTIES PASS_REGULAR_EXPRESSION "0x[0-9A-Fa-f]+:00000000")

  add_test(
    NAME holyc.jit.decl-modifiers
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/decl_modifiers.HC" --permissive
  )
  set_tests_properties(holyc.jit.decl-modifiers PROPERTIES PASS_REGULAR_EXPRESSION "3")

  add_test(
    NAME holyc.jit.lock-stmt
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/lock_stmt.HC"
  )
  set_tests_properties(holyc.jit.lock-stmt PROPERTIES PASS_REGULAR_EXPRESSION "4")

  add_test(
    NAME holyc.jit.default-arg-expr
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/default_arg_expr.HC"
  )
  set_tests_properties(holyc.jit.default-arg-expr PROPERTIES PASS_REGULAR_EXPRESSION "15")

  add_test(
    NAME holyc.jit.static-function-linkage
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/static_function_linkage.HC"
  )
  set_tests_properties(holyc.jit.static-function-linkage PROPERTIES PASS_REGULAR_EXPRESSION "5")

  add_test(
    NAME holyc.jit.extern-global-redeclaration
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/extern_global_redeclaration.HC"
  )
  set_tests_properties(holyc.jit.extern-global-redeclaration PROPERTIES PASS_REGULAR_EXPRESSION "7")

  if(HOLYC_LLVM_ENABLED)
    add_test(
      NAME holyc.jit.llvm.print-runtime-formatting
      COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/print_runtime_formatting.HC" --jit-backend=llvm
    )
    set_tests_properties(holyc.jit.llvm.print-runtime-formatting PROPERTIES PASS_REGULAR_EXPRESSION "T3 ok Z 7")

    add_test(
      NAME holyc.jit.llvm.lock-stmt
      COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/lock_stmt.HC" --jit-backend=llvm
    )
    set_tests_properties(holyc.jit.llvm.lock-stmt PROPERTIES PASS_REGULAR_EXPRESSION "4")

    add_test(
      NAME holyc.jit.llvm.constexpr-global-init
      COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/constexpr_global_init.HC" --jit-backend=llvm
    )
    set_tests_properties(holyc.jit.llvm.constexpr-global-init PROPERTIES PASS_REGULAR_EXPRESSION "8")

    add_test(
      NAME holyc.jit.llvm.callstkgrow-runtime
      COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/callstkgrow_runtime.HC" --jit-backend=llvm
    )
    set_tests_properties(holyc.jit.llvm.callstkgrow-runtime PROPERTIES PASS_REGULAR_EXPRESSION "5")

    add_test(
      NAME holyc.jit.llvm.job-queue-runtime
      COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/job_queue_runtime.HC" --jit-backend=llvm
    )
    set_tests_properties(holyc.jit.llvm.job-queue-runtime PROPERTIES PASS_REGULAR_EXPRESSION "2")

    add_test(
      NAME holyc.jit.llvm.metadata-runtime-apis
      COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/metadata_runtime_apis.HC" --jit-backend=llvm
    )
    set_tests_properties(holyc.jit.llvm.metadata-runtime-apis PROPERTIES PASS_REGULAR_EXPRESSION "7")

    add_test(
      NAME holyc.jit.llvm.inline-asm-nop
      COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/inline_asm_nop.HC" --jit-backend=llvm
    )
    set_tests_properties(holyc.jit.llvm.inline-asm-nop PROPERTIES PASS_REGULAR_EXPRESSION "83")

    add_test(
      NAME holyc.jit.llvm.main-with-params
      COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/main_with_params.HC" --jit-backend=llvm
    )
    set_tests_properties(holyc.jit.llvm.main-with-params PROPERTIES PASS_REGULAR_EXPRESSION "0")

    add_test(
      NAME holyc.jit.llvm.print-star-width-precision
      COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/print_star_width_precision.HC" --jit-backend=llvm
    )
    set_tests_properties(holyc.jit.llvm.print-star-width-precision PROPERTIES PASS_REGULAR_EXPRESSION "042:.*ok")

    add_test(
      NAME holyc.jit.llvm.anon-aggregate-trailing
      COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/anon_aggregate_trailing_decl.HC" --jit-backend=llvm
    )
    set_tests_properties(holyc.jit.llvm.anon-aggregate-trailing PROPERTIES PASS_REGULAR_EXPRESSION "6")

    add_test(
      NAME holyc.jit.llvm.indirect-call-forms
      COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/indirect_call_forms.HC" --jit-backend=llvm
    )
    set_tests_properties(holyc.jit.llvm.indirect-call-forms PROPERTIES PASS_REGULAR_EXPRESSION "43")

    add_test(
      NAME holyc.jit.llvm.inline-asm-multi-output
      COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/inline_asm_multi_output.HC" --jit-backend=llvm
    )
    set_tests_properties(holyc.jit.llvm.inline-asm-multi-output PROPERTIES PASS_REGULAR_EXPRESSION "88")

    add_test(
      NAME holyc.jit.llvm.subint-lane-access
      COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/subint_lane_access.HC" --jit-backend=llvm
    )
    set_tests_properties(holyc.jit.llvm.subint-lane-access PROPERTIES PASS_REGULAR_EXPRESSION "50")

    add_test(
      NAME holyc.jit.llvm.lane-assignments
      COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/lane_assignments.HC" --jit-backend=llvm
    )
    set_tests_properties(holyc.jit.llvm.lane-assignments PROPERTIES PASS_REGULAR_EXPRESSION "12")

    add_test(
      NAME holyc.jit.llvm.inline-asm-input-operand
      COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/inline_asm_input_operand.HC" --jit-backend=llvm
    )
    set_tests_properties(holyc.jit.llvm.inline-asm-input-operand PROPERTIES PASS_REGULAR_EXPRESSION "5")
  endif()

  add_test(
    NAME holyc.run.adjacent-string-literals
    COMMAND $<TARGET_FILE:holyc> run "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/adjacent_string_literals.HC"
  )
  set_tests_properties(holyc.run.adjacent-string-literals PROPERTIES PASS_REGULAR_EXPRESSION "Hello, HolyC")

  add_test(
    NAME holyc.run.dollar-expr
    COMMAND $<TARGET_FILE:holyc> run "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/dollar_expr.HC"
  )
  set_tests_properties(holyc.run.dollar-expr PROPERTIES PASS_REGULAR_EXPRESSION "0x[0-9A-Fa-f]+:00000000")

  if(HOLYC_LLVM_ENABLED)
    add_test(
      NAME holyc.run.print-runtime-formatting
      COMMAND $<TARGET_FILE:holyc> run "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/print_runtime_formatting.HC"
    )
    set_tests_properties(holyc.run.print-runtime-formatting PROPERTIES PASS_REGULAR_EXPRESSION "T3 ok Z 7")
  endif()

  add_test(
    NAME holyc.build.expr-ops
    COMMAND $<TARGET_FILE:holyc> build "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/expr_ops.HC" -o "${CMAKE_CURRENT_BINARY_DIR}/expr_ops"
  )

  add_test(
    NAME holyc.emit-llvm.preprocess-modes
    COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_modes.HC"
  )
  set_tests_properties(holyc.emit-llvm.preprocess-modes PROPERTIES PASS_REGULAR_EXPRESSION "ret i64 20")

  add_test(
    NAME holyc.emit-llvm.preprocess-modes-jit
    COMMAND $<TARGET_FILE:holyc> emit-llvm "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_modes.HC" --mode=jit
  )
  set_tests_properties(holyc.emit-llvm.preprocess-modes-jit PROPERTIES PASS_REGULAR_EXPRESSION "ret i64 10")

  add_test(
    NAME holyc.jit.preprocess-modes
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_modes.HC"
  )
  set_tests_properties(holyc.jit.preprocess-modes PROPERTIES PASS_REGULAR_EXPRESSION "10")

  add_test(
    NAME holyc.preprocess.preprocess-modes-jit
    COMMAND $<TARGET_FILE:holyc> preprocess "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_modes.HC" --mode=jit
  )
  set_tests_properties(holyc.preprocess.preprocess-modes-jit PROPERTIES PASS_REGULAR_EXPRESSION "return 10")

  add_test(
    NAME holyc.preprocess.preprocess-modes-aot
    COMMAND $<TARGET_FILE:holyc> preprocess "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_modes.HC" --mode=aot
  )
  set_tests_properties(holyc.preprocess.preprocess-modes-aot PROPERTIES PASS_REGULAR_EXPRESSION "return 20")

  add_test(
    NAME holyc.ast-dump.preprocess-modes-jit
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_modes.HC" --mode=jit
  )
  set_tests_properties(holyc.ast-dump.preprocess-modes-jit PROPERTIES PASS_REGULAR_EXPRESSION "Literal: 10")

  add_test(
    NAME holyc.ast-dump.preprocess-modes-aot
    COMMAND $<TARGET_FILE:holyc> ast-dump "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_modes.HC" --mode=aot
  )
  set_tests_properties(holyc.ast-dump.preprocess-modes-aot PROPERTIES PASS_REGULAR_EXPRESSION "Literal: 20")

  add_test(
    NAME holyc.preprocess.ifexpr
    COMMAND $<TARGET_FILE:holyc> preprocess "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_ifexpr.HC"
  )
  set_tests_properties(holyc.preprocess.ifexpr PROPERTIES PASS_REGULAR_EXPRESSION "return 42")

  add_test(
    NAME holyc.jit.ifexpr
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_ifexpr.HC"
  )
  set_tests_properties(holyc.jit.ifexpr PROPERTIES PASS_REGULAR_EXPRESSION "42")

  add_test(
    NAME holyc.jit.function-macros
    COMMAND $<TARGET_FILE:holyc> jit "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_function_macros.HC"
  )
  set_tests_properties(holyc.jit.function-macros PROPERTIES PASS_REGULAR_EXPRESSION "4")

  add_test(
    NAME holyc.build.ifexpr
    COMMAND $<TARGET_FILE:holyc> build "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_ifexpr.HC" -o "${CMAKE_CURRENT_BINARY_DIR}/ifexpr"
  )

  add_test(
    NAME holyc.build.function-macros
    COMMAND $<TARGET_FILE:holyc> build "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_function_macros.HC" -o "${CMAKE_CURRENT_BINARY_DIR}/function_macros"
  )

  add_test(
    NAME holyc.build.invalid-aot-trycatch
    COMMAND $<TARGET_FILE:holyc> build "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/invalid_aot_trycatch.HC" -o "${CMAKE_CURRENT_BINARY_DIR}/aot_should_fail"
  )

  add_test(
    NAME holyc.diff.llvm
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/llvm.HC"
  )

  add_test(
    NAME holyc.diff.switch-compat
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/switch_compat.HC"
  )

  add_test(
    NAME holyc.diff.default-args
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/default_args_lowering.HC"
  )

  add_test(
    NAME holyc.diff.control-flow
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/control_flow.HC"
  )

  add_test(
    NAME holyc.diff.runtime-print
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/runtime_print.HC"
  )

  add_test(
    NAME holyc.diff.expr-ops
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/expr_ops.HC"
  )

  add_test(
    NAME holyc.diff.preprocess-ifexpr
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_ifexpr.HC"
  )

  add_test(
    NAME holyc.diff.function-macros
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/preprocess_function_macros.HC"
  )

  add_test(
    NAME holyc.diff.linkage-validation-ok
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/linkage_validation_ok.HC"
  )

  add_test(
    NAME holyc.diff.runtime-try
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/runtime_try.HC"
  )

  add_test(
    NAME holyc.diff.reflection-runtime
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/reflection_runtime.HC"
  )

  add_test(
    NAME holyc.diff.switch-edge-cases
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/switch_edge_cases.HC"
  )

  add_test(
    NAME holyc.diff.switch-start-end-edge
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/switch_start_end_edge.HC"
  )

  add_test(
    NAME holyc.diff.adjacent-string-literals
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/adjacent_string_literals.HC"
  )

  if(HOLYC_LLVM_ENABLED)
    add_test(
      NAME holyc.diff.lock-stmt
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/lock_stmt.HC"
    )

    add_test(
      NAME holyc.diff.print-runtime-formatting
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/print_runtime_formatting.HC"
    )

    add_test(
      NAME holyc.diff.extern-global-redeclaration
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/extern_global_redeclaration.HC"
    )

    add_test(
      NAME holyc.diff.constexpr-global-init
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/constexpr_global_init.HC"
    )

    add_test(
      NAME holyc.diff.callstkgrow-runtime
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/callstkgrow_runtime.HC"
    )

    add_test(
      NAME holyc.diff.job-queue-runtime
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/job_queue_runtime.HC"
    )

    add_test(
      NAME holyc.diff.metadata-runtime-apis
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/metadata_runtime_apis.HC"
    )

    add_test(
      NAME holyc.diff.inline-asm-nop
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/differential/run_diff_test.sh" $<TARGET_FILE:holyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/inline_asm_nop.HC"
    )
  endif()

  add_test(
    NAME holyc.repro.emit-llvm.llvm
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/repro/run_emit_llvm_repro.sh"
            $<TARGET_FILE:holyc>
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/llvm.HC"
  )

  if(HOLYC_LLVM_ENABLED)
    add_test(
      NAME holyc.repro.build.llvm
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/repro/run_build_repro.sh"
              $<TARGET_FILE:holyc>
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/samples/llvm.HC"
    )
  endif()

  add_test(
    NAME holyc.stress.deep-include-graph
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/stress/run_deep_include_graph.sh"
            $<TARGET_FILE:holyc>
  )

  add_test(
    NAME holyc.hardening.malformed-inputs
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/hardening/run_malformed_inputs.sh"
            $<TARGET_FILE:holyc>
  )

  add_test(
    NAME holyc.corpus.holyc-docs.curated
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/run_holyc_docs_curated.sh"
            $<TARGET_FILE:holyc>
  )
endif()
