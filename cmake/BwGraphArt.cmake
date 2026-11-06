set(BWGRAPH_ART_ROOT
    "${PROJECT_SOURCE_DIR}/third_party/art-vertex-index")

if(NOT EXISTS "${BWGRAPH_ART_ROOT}/OptimisticLockCoupling/Tree.cpp")
  message(FATAL_ERROR
    "ART sources missing. Run: git submodule update --init --recursive")
endif()

find_package(TBB CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(bwgraph_art STATIC
  "${BWGRAPH_ART_ROOT}/OptimisticLockCoupling/Tree.cpp"
)

target_compile_features(bwgraph_art PUBLIC cxx_std_20)

target_include_directories(bwgraph_art PUBLIC
  "${BWGRAPH_ART_ROOT}"
)

target_link_libraries(bwgraph_art PUBLIC
  TBB::tbb
  Threads::Threads
)

set_target_properties(bwgraph_art PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

add_executable(test_art_compat "${PROJECT_SOURCE_DIR}/test/third/art_compat.cc")
target_link_libraries(test_art_compat PRIVATE bwgraph_art)
add_test(NAME test_art_compat COMMAND test_art_compat)
