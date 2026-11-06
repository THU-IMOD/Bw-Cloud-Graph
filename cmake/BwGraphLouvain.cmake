set(BWGRAPH_LOUVAIN_ROOT "${PROJECT_SOURCE_DIR}/third_party/louvain-communities-openmp")
if(NOT EXISTS "${BWGRAPH_LOUVAIN_ROOT}/inc/louvain.hxx")
  message(FATAL_ERROR "Louvain sources missing. Run: git submodule update --init --recursive")
endif()

find_package(OpenMP REQUIRED COMPONENTS CXX)
add_library(bwgraph_louvain INTERFACE)
target_compile_features(bwgraph_louvain INTERFACE cxx_std_20)
# Keep diagnostics enabled for Bw-Graph while treating the pinned dependency as external.
target_include_directories(bwgraph_louvain SYSTEM INTERFACE "${BWGRAPH_LOUVAIN_ROOT}/inc")
target_compile_definitions(bwgraph_louvain INTERFACE OPENMP)
target_link_libraries(bwgraph_louvain INTERFACE OpenMP::OpenMP_CXX)

add_executable(test_louvain_compat "${PROJECT_SOURCE_DIR}/test/third/louvain_compat.cc")
target_link_libraries(test_louvain_compat PRIVATE bwgraph_louvain GTest::gtest_main)
add_test(NAME test_louvain_compat COMMAND test_louvain_compat)
set_tests_properties(test_louvain_compat PROPERTIES TIMEOUT 60)
