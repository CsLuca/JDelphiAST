cmake_minimum_required(VERSION 3.20)
project(JDelphiAST VERSION 0.1.0 LANGUAGES CXX)

option(JDELPHIAST_BUILD_TESTS "Build JDelphiAST tests" ON)

add_library(jdelphiast src/analyzer.cpp)
target_include_directories(jdelphiast PUBLIC include)
target_compile_features(jdelphiast PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(jdelphiast PRIVATE /W4 /permissive-)
else()
  target_compile_options(jdelphiast PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(jdelphiast_cli src/main.cpp)
target_link_libraries(jdelphiast_cli PRIVATE jdelphiast)

if(JDELPHIAST_BUILD_TESTS)
  enable_testing()
  add_executable(jdelphiast_tests tests/analyzer_tests.cpp)
  target_link_libraries(jdelphiast_tests PRIVATE jdelphiast)
  add_test(NAME jdelphiast_tests COMMAND jdelphiast_tests)
endif()
