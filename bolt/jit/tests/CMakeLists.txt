#
# Copyright (c) ByteDance Ltd. and/or its affiliates
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

if(${ENABLE_BOLT_JIT})

  add_executable(bolt_thrustjit_test RowContainerIRTest.cpp ThrustJitEngineTest.cpp )
  target_compile_features(bolt_thrustjit_test PUBLIC cxx_std_20)
  target_link_libraries(
    bolt_thrustjit_test bolt_process bolt_thrustjit bolt_exception GTest::gtest GTest::gtest_main
  )

  add_executable(bolt_rowEqVectors_test RowEqVectorsIRTest.cpp)
  target_link_libraries(
    bolt_rowEqVectors_test
    bolt_process
    bolt_exec
    bolt_type
    bolt_functions_lib
    bolt_exec_test_lib
    bolt_functions_prestosql
    bolt_vector_fuzzer
    bolt_vector_test_lib
    Folly::folly
    bolt_thrustjit
    GTest::gtest
    GTest::gtest_main
  )

  add_executable(bolt_rowEqRow_test RowEqRowIRTest.cpp)

  target_link_libraries(
    bolt_rowEqRow_test
    bolt_process
    bolt_exec
    bolt_type
    bolt_functions_lib
    bolt_exec_test_lib
    bolt_functions_prestosql
    bolt_vector_fuzzer
    bolt_vector_test_lib
    Folly::folly
    bolt_thrustjit
    GTest::gtest
    GTest::gtest_main
  )

  set_target_properties(bolt_thrustjit_test PROPERTIES ENABLE_EXPORTS ON)

  add_test(bolt_thrustjit_test bolt_thrustjit_test)

endif()

# TODO: refactor expression codegen
if(${ENABLE_BOLT_EXPR_JIT})
  add_executable(bolt_expr_jit_test JitExprTest.cpp)
  find_package(ThrustVector REQUIRED)
  add_test(
    NAME bolt_expr_jit_test
    COMMAND bolt_expr_jit_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )

  target_compile_features(bolt_expr_jit_test PUBLIC cxx_std_17)

  target_link_libraries(
    bolt_expr_jit_test
    bolt_functions_test_lib
    bolt_exec
    bolt_dwio_common
    bolt_aggregates
    bolt_aggregates_test_lib
    bolt_functions_lib
    bolt_functions_prestosql
    bolt_hive_connector
    bolt_type
    bolt_serialization
    bolt_exec_test_util
    bolt_functions_json
    bolt_vector
    bolt_memory
    bolt_dwio_common_exception
    ThrustVector::JIT
    ${Boost_ATOMIC_LIBRARIES}
    ${Boost_CONTEXT_LIBRARIES}
    ${Boost_DATE_TIME_LIBRARIES}
    ${Boost_FILESYSTEM_LIBRARIES}
    ${Boost_PROGRAM_OPTIONS_LIBRARIES}
    ${Boost_REGEX_LIBRARIES}
    ${Boost_THREAD_LIBRARIES}
    ${Boost_SYSTEM_LIBRARIES}
    ${FOLLY_WITH_DEPENDENCIES}
    ${FOLLY_BENCHMARK}
    ${DOUBLE_CONVERSION}
    ${gflags_LIBRARIES}
    bolt_process
    glog::glog
    ${FMT}
    ${FILESYSTEM}
  )

endif() # ENABLE_BOLT_EXPR_JIT
