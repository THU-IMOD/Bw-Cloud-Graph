cmake_minimum_required(VERSION 3.14)
project(BwGraphStorageValidation LANGUAGES CXX)
# Independent raw I/O checks: this is NOT a full Bw-Graph configuration.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(BWGRAPH_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_package(Threads REQUIRED)
find_package(GTest REQUIRED)
add_library(storage_under_test "${BWGRAPH_SOURCE}/src/multiscale_disk_manager.cc")
target_include_directories(storage_under_test PUBLIC "${BWGRAPH_SOURCE}/include")
target_link_libraries(storage_under_test PUBLIC Threads::Threads)
target_compile_options(storage_under_test PRIVATE -Wall -Wextra -Wpedantic)
enable_testing()
add_executable(test_multiscale_disk_manager
  "${BWGRAPH_SOURCE}/test/storage/test_multiscale_disk_manager.cc")
target_link_libraries(test_multiscale_disk_manager PRIVATE storage_under_test GTest::gtest_main)
add_test(NAME test_multiscale_disk_manager COMMAND test_multiscale_disk_manager)
add_executable(multiscale_disk_bench "${BWGRAPH_SOURCE}/bw-test/multiscale_disk_bench.cc")
target_link_libraries(multiscale_disk_bench PRIVATE storage_under_test)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(test_multiscale_linux_io
    "${BWGRAPH_SOURCE}/test/storage/test_multiscale_linux_io.cc")
  target_link_libraries(test_multiscale_linux_io PRIVATE storage_under_test GTest::gtest_main)
  target_link_options(test_multiscale_linux_io PRIVATE
    -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fdatasync)
  add_test(NAME test_multiscale_linux_io COMMAND test_multiscale_linux_io)
endif()
