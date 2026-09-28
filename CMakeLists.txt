cmake_minimum_required(VERSION 3.20)
project(JDelphiAST VERSION 0.1.0 LANGUAGES CXX)

option(JDELPHIAST_BUILD_TESTS "Build JDelphiAST tests" ON)

add_library(jdelphiast src/analyzer.cpp src/project.cpp src/preprocessor.cpp src/queries.cpp)
target_include_directories(jdelphiast PUBLIC include)
target_compile_features(jdelphiast PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(jdelphiast PRIVATE /W4 /permissive-)
else()
  target_compile_options(jdelphiast PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(DelphiAstTool src/main.cpp)
target_link_libraries(DelphiAstTool PRIVATE jdelphiast)
if(MINGW)
  # Keep the distributed CLI independent from the MSYS2 GCC runtime DLLs.
  target_link_options(DelphiAstTool PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_custom_command(TARGET DelphiAstTool POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/indexes
          $<TARGET_FILE_DIR:DelphiAstTool>/indexes)

if(JDELPHIAST_BUILD_TESTS)
  enable_testing()
  add_executable(jdelphiast_tests tests/analyzer_tests.cpp)
  target_link_libraries(jdelphiast_tests PRIVATE jdelphiast)
  add_test(NAME jdelphiast_tests COMMAND jdelphiast_tests)
  add_executable(jdelphiast_project_tests tests/project_tests.cpp)
  target_link_libraries(jdelphiast_project_tests PRIVATE jdelphiast)
  add_test(NAME jdelphiast_project_tests COMMAND jdelphiast_project_tests)
  add_executable(jdelphiast_preprocessor_tests tests/preprocessor_tests.cpp)
  target_link_libraries(jdelphiast_preprocessor_tests PRIVATE jdelphiast)
  add_test(NAME jdelphiast_preprocessor_tests COMMAND jdelphiast_preprocessor_tests)
  add_executable(jdelphiast_query_tests tests/query_tests.cpp)
  target_link_libraries(jdelphiast_query_tests PRIVATE jdelphiast)
  target_compile_definitions(jdelphiast_query_tests PRIVATE
    JDELPHIAST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    JDELPHIAST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME jdelphiast_query_tests COMMAND jdelphiast_query_tests)
  add_test(NAME jdelphiast_cli_package
    COMMAND DelphiAstTool --project ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/DemoPackage.dpk
            --output ${CMAKE_CURRENT_BINARY_DIR}/DemoPackage.ast.json)
  set_tests_properties(jdelphiast_cli_package PROPERTIES
    PASS_REGULAR_EXPRESSION "Wrote.*DemoPackage.ast.json")
  add_test(NAME jdelphiast_cli_requires_output
    COMMAND DelphiAstTool --project ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/DemoPackage.dpk)
  set_tests_properties(jdelphiast_cli_requires_output PROPERTIES WILL_FAIL TRUE)
  add_test(NAME jdelphiast_cli_index
    COMMAND DelphiAstTool index --source ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/shared
            --output ${CMAKE_CURRENT_BINARY_DIR}/generated-v600.jdi)
  set_tests_properties(jdelphiast_cli_index PROPERTIES PASS_REGULAR_EXPRESSION "Wrote.*generated-v600.jdi")
  add_test(NAME jdelphiast_cli_index_resilient
    COMMAND DelphiAstTool index --source ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/shared
            --output ${CMAKE_CURRENT_BINARY_DIR}/generated-resilient.jdi --version v600 --format json)
  set_tests_properties(jdelphiast_cli_index_resilient PROPERTIES
    PASS_REGULAR_EXPRESSION "files_skipped_non_unit_source.*no_unit_declaration")
  add_test(NAME jdelphiast_cli_index_strict_validation
    COMMAND DelphiAstTool index --source ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/shared
            --output ${CMAKE_CURRENT_BINARY_DIR}/generated-strict.jdi --version v600 --format json
            --validation-policy ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/strict-validation.policy)
  set_tests_properties(jdelphiast_cli_index_strict_validation PROPERTIES WILL_FAIL TRUE)
  add_test(NAME jdelphiast_cli_index_source_root
    COMMAND DelphiAstTool index --source-root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/shared
            --output ${CMAKE_CURRENT_BINARY_DIR}/generated-source-root.jdi --version v600 --format json)
  set_tests_properties(jdelphiast_cli_index_source_root PROPERTIES PASS_REGULAR_EXPRESSION "units_indexed")
  add_test(NAME jdelphiast_cli_index_info_policy
    COMMAND DelphiAstTool index --source-root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/shared
            --output ${CMAKE_CURRENT_BINARY_DIR}/generated-info.jdi --version v600 --format json
            --validation-policy ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/info-validation.policy)
  set_tests_properties(jdelphiast_cli_index_info_policy PROPERTIES
    PASS_REGULAR_EXPRESSION "info_count.*1")
  add_test(NAME jdelphiast_cli_index_incomplete_unit
    COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:DelphiAstTool> -DEXPECT_EXIT=0 -DEXPECT_TEXT=incomplete_unit
      -DEXPECT_PRESENT_FILE=${CMAKE_CURRENT_BINARY_DIR}/incomplete-unit.jdi
      -DEXPECT_FILE_TEXT=confidence=low.*diagnostics=incomplete_unit
      "-DARGS=index;--source;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/incomplete;--output;${CMAKE_CURRENT_BINARY_DIR}/incomplete-unit.jdi;--version;v600;--format;json"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_command.cmake)
  add_test(NAME jdelphiast_cli_index_missing_root
    COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:DelphiAstTool> -DEXPECT_EXIT=2 -DEXPECT_TEXT=missing_source_root
      -DEXPECT_MISSING_FILE=${CMAKE_CURRENT_BINARY_DIR}/missing-root.jdi -DCREATE_STALE_FILE=1
      "-DARGS=index;--source-root;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/does-not-exist;--output;${CMAKE_CURRENT_BINARY_DIR}/missing-root.jdi;--version;v600;--format;json"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_command.cmake)
  add_test(NAME jdelphiast_cli_analyze_v2
    COMMAND DelphiAstTool analyze --dproj ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/DemoPackage.dproj
            --config Release --platform Win32 --format json)
  set_tests_properties(jdelphiast_cli_analyze_v2 PROPERTIES PASS_REGULAR_EXPRESSION "schema_version.*2.1")
  add_test(NAME jdelphiast_cli_exports
    COMMAND DelphiAstTool exports --unit CSControls.Theme --index ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/v600.jdi --format json)
  set_tests_properties(jdelphiast_cli_exports PROPERTIES PASS_REGULAR_EXPRESSION "CSGlobalTheme")
  add_test(NAME jdelphiast_cli_symbol
    COMMAND DelphiAstTool symbol --name RegCsProc --index ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/v600.jdi --format json)
  set_tests_properties(jdelphiast_cli_symbol PROPERTIES PASS_REGULAR_EXPRESSION "RegCsProc")
  add_test(NAME jdelphiast_cli_expression
    COMMAND DelphiAstTool expression --file ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ExpressionSample.pas --line 14
            --index ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/v600.jdi --format json)
  set_tests_properties(jdelphiast_cli_expression PROPERTIES PASS_REGULAR_EXPRESSION "TFDParam")
  add_test(NAME jdelphiast_cli_hierarchy
    COMMAND DelphiAstTool hierarchy --file ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/HierarchySample.pas
            --class TOExtArtControCamp --index ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/v600.jdi --format json)
  set_tests_properties(jdelphiast_cli_hierarchy PROPERTIES PASS_REGULAR_EXPRESSION "override_signature_incompatible")
  add_test(NAME jdelphiast_cli_unit_info
    COMMAND DelphiAstTool unit-info --unit UMarketing --index ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/v600.jdi --format json)
  set_tests_properties(jdelphiast_cli_unit_info PROPERTIES PASS_REGULAR_EXPRESSION "package_metadata_not_available")
  add_test(NAME jdelphiast_cli_compare
    COMMAND DelphiAstTool compare-symbol --left-index ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/v500.jdi
            --right-index ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/v600.jdi --symbol UCSTheme --format json)
  set_tests_properties(jdelphiast_cli_compare PROPERTIES PASS_REGULAR_EXPRESSION "partial_compatible")
  add_test(NAME jdelphiast_cli_query_error_json
    COMMAND DelphiAstTool symbol --name Missing --index ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/missing.jdi --format json)
  set_tests_properties(jdelphiast_cli_query_error_json PROPERTIES PASS_REGULAR_EXPRESSION "query_execution_failed")
  add_test(NAME jdelphiast_cli_help COMMAND DelphiAstTool /?)
  set_tests_properties(jdelphiast_cli_help PROPERTIES
    PASS_REGULAR_EXPRESSION "SAFE USES REMOVAL.*PREPROCESSING.*PERSISTENT INDEX")
  add_test(NAME jdelphiast_cli_help_word COMMAND DelphiAstTool help)
  set_tests_properties(jdelphiast_cli_help_word PROPERTIES PASS_REGULAR_EXPRESSION "DelphiAstTool.*USAGE")
  add_test(NAME jdelphiast_cli_compare_units
    COMMAND DelphiAstTool compare-units
      --left-index ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unit-map-v500.jdi
      --right-index ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unit-map-v600.jdi
      --seed ${CMAKE_CURRENT_SOURCE_DIR}/mappings/uses_mapping_seed.json
      --output ${CMAKE_CURRENT_BINARY_DIR}/uses-map.json
      --csv ${CMAKE_CURRENT_BINARY_DIR}/uses-map.csv
      --html ${CMAKE_CURRENT_BINARY_DIR}/uses-map.html
      --include-unmapped --include-ambiguous --include-symbol-details --format json)
  set_tests_properties(jdelphiast_cli_compare_units PROPERTIES PASS_REGULAR_EXPRESSION "CSETables.*CSData.CSETables")
  add_test(NAME jdelphiast_cli_compare_units_malformed
    COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:DelphiAstTool>
      -DEXPECT_EXIT=2
      -DEXPECT_TEXT=index_parse_failed
      -DEXPECT_MISSING_FILE=${CMAKE_CURRENT_BINARY_DIR}/invalid-map.json
      -DCREATE_STALE_FILE=1
      "-DARGS=compare-units;--left-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/malformed.jdi;--right-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unit-map-v600.jdi;--output;${CMAKE_CURRENT_BINARY_DIR}/invalid-map.json;--format;json"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_command.cmake)
  add_test(NAME jdelphiast_cli_compare_units_duplicate
    COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:DelphiAstTool>
      -DEXPECT_EXIT=0
      -DEXPECT_TEXT=ambiguous
      -DEXPECT_PRESENT_FILE=${CMAKE_CURRENT_BINARY_DIR}/duplicate-map.json
      "-DARGS=compare-units;--left-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/duplicate-unit.jdi;--right-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unit-map-v600.jdi;--output;${CMAKE_CURRENT_BINARY_DIR}/duplicate-map.json;--include-ambiguous;--format;json"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_command.cmake)
  add_test(NAME jdelphiast_cli_compare_units_duplicate_symbol
    COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:DelphiAstTool> -DEXPECT_EXIT=2 -DEXPECT_TEXT=duplicate_symbol
      "-DARGS=compare-units;--left-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/duplicate-symbol.jdi;--right-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unit-map-v600.jdi;--output;${CMAKE_CURRENT_BINARY_DIR}/duplicate-symbol-map.json;--format;json"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_command.cmake)
  add_test(NAME jdelphiast_cli_compare_units_mixed_version
    COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:DelphiAstTool> -DEXPECT_EXIT=2 -DEXPECT_TEXT=catalog_version_inconsistent
      "-DARGS=compare-units;--left-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mixed-version.jdi;--right-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unit-map-v600.jdi;--output;${CMAKE_CURRENT_BINARY_DIR}/mixed-map.json;--format;json"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_command.cmake)
  add_test(NAME jdelphiast_cli_compare_units_bad_package
    COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:DelphiAstTool> -DEXPECT_EXIT=2 -DEXPECT_TEXT=package_metadata_inconsistent
      "-DARGS=compare-units;--left-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bad-package.jdi;--right-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unit-map-v600.jdi;--output;${CMAKE_CURRENT_BINARY_DIR}/bad-package-map.json;--format;json"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_command.cmake)
  add_test(NAME jdelphiast_cli_compare_units_empty
    COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:DelphiAstTool> -DEXPECT_EXIT=2 -DEXPECT_TEXT=no_input_units
      "-DARGS=compare-units;--left-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/empty.jdi;--right-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unit-map-v600.jdi;--output;${CMAKE_CURRENT_BINARY_DIR}/empty-map.json;--format;json"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_command.cmake)
  add_test(NAME jdelphiast_cli_compare_units_bad_seed
    COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:DelphiAstTool> -DEXPECT_EXIT=2 -DEXPECT_TEXT=seed_parse_failed
      "-DARGS=compare-units;--left-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unit-map-v500.jdi;--right-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unit-map-v600.jdi;--seed;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bad-seed.json;--output;${CMAKE_CURRENT_BINARY_DIR}/bad-seed-map.json;--format;json"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_command.cmake)
  add_test(NAME jdelphiast_cli_compare_units_warning_policy
    COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:DelphiAstTool> -DEXPECT_EXIT=0 -DEXPECT_TEXT=has_blocking_errors.*false
      "-DARGS=compare-units;--left-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unit-map-v500.jdi;--right-index;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unit-map-v600.jdi;--seed;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/missing-target-seed.json;--validation-policy;${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/warning-validation.policy;--output;${CMAKE_CURRENT_BINARY_DIR}/warning-policy-map.json;--include-unmapped;--format;json"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_command.cmake)
endif()
