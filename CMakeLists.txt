cmake_minimum_required(VERSION 3.20)
project(JDelphiAST VERSION 0.1.0 LANGUAGES CXX)

option(JDELPHIAST_BUILD_TESTS "Build JDelphiAST tests" ON)

add_library(jdelphiast src/analyzer.cpp src/project.cpp)
target_include_directories(jdelphiast PUBLIC include)
target_compile_features(jdelphiast PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(jdelphiast PRIVATE /W4 /permissive-)
else()
  target_compile_options(jdelphiast PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(jdelphiast_cli src/main.cpp)
target_link_libraries(jdelphiast_cli PRIVATE jdelphiast)
add_custom_command(TARGET jdelphiast_cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/indexes
          $<TARGET_FILE_DIR:jdelphiast_cli>/indexes)

if(JDELPHIAST_BUILD_TESTS)
  enable_testing()
  add_executable(jdelphiast_tests tests/analyzer_tests.cpp)
  target_link_libraries(jdelphiast_tests PRIVATE jdelphiast)
  add_test(NAME jdelphiast_tests COMMAND jdelphiast_tests)
  add_executable(jdelphiast_project_tests tests/project_tests.cpp)
  target_link_libraries(jdelphiast_project_tests PRIVATE jdelphiast)
  add_test(NAME jdelphiast_project_tests COMMAND jdelphiast_project_tests)
  add_test(NAME jdelphiast_cli_package
    COMMAND jdelphiast_cli --json ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/DemoPackage.dpk)
  set_tests_properties(jdelphiast_cli_package PROPERTIES
    PASS_REGULAR_EXPRESSION "SysUtils.*USED")
endif()
