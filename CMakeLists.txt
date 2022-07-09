cmake_minimum_required(VERSION 3.10)

project(yaoyao VERSION 0.1)

set(LLVM_DIR /home/yimin/.local/lib/cmake/llvm/)
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found llvm ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in :${LLVM_DIR}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS "-fno-rtti")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(llvm_libs
  Analysis
  Core
  ExecutionEngine
  InstCombine
  Object
  OrcError
  OrcJIT
  RuntimeDyld
  ScalarOpts
  Support
  TransformUtils
  native)

add_executable(yaolang src/yaolang.cpp)
add_executable(JITServer src/JITServer.cpp)

add_subdirectory(src)
list(APPEND EXTRA_LIBS ${llvm_libs})
list(APPEND EXTRA_INCLUDES src include)

target_include_directories(yaolang PUBLIC ${EXTRA_INCLUDES})
target_link_libraries(yaolang PUBLIC ${EXTRA_LIBS})

target_include_directories(JITServer PUBLIC ${EXTRA_INCLUDES})
target_link_libraries(JITServer PUBLIC ${EXTRA_LIBS})