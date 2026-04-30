cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(dtx LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

set(DTX_MAIN_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(runtime/cpp-llvm)
add_subdirectory(tools/dtx-codegen)

set(DTX_INCLUDE_TESTS_DEFAULT ON)
if(DEFINED LLVM_INCLUDE_TESTS)
  set(DTX_INCLUDE_TESTS_DEFAULT ${LLVM_INCLUDE_TESTS})
endif()
option(DTX_INCLUDE_TESTS "Build DTX tests" ${DTX_INCLUDE_TESTS_DEFAULT})

if(DTX_INCLUDE_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
