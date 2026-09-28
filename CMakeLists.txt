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
    JDELPHIAST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
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
  add_test(NAME jdelphiast_cli_analyze_v2
    COMMAND DelphiAstTool analyze --dproj ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/DemoPackage.dproj
            --config Release --platform Win32 --format json)
  set_tests_properties(jdelphiast_cli_analyze_v2 PROPERTIES PASS_REGULAR_EXPRESSION "schema_version.*2.0")
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
endif()
