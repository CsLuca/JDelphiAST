cmake_minimum_required(VERSION 3.20)
project(JDelphiAST VERSION 0.1.0 LANGUAGES CXX)

option(JDELPHIAST_BUILD_TESTS "Build JDelphiAST tests" ON)

add_library(jdelphiast src/analyzer.cpp src/project.cpp src/preprocessor.cpp)
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
endif()
