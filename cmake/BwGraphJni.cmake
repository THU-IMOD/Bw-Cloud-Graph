option(BW_GRAPH_BUILD_JNI "Build the optional Java 17 JNI adapter and JVM tests" OFF)
if(NOT BW_GRAPH_BUILD_JNI)
  return()
endif()
find_package(Java 17 REQUIRED COMPONENTS Development Runtime)
find_package(JNI REQUIRED)
set(BW_JAVA_SOURCE "${PROJECT_SOURCE_DIR}/bindings/java/db/monacgraph/jni/BwGraphJNI.java")
set(BW_JAVA_TEST "${PROJECT_SOURCE_DIR}/bindings/java/test/BwGraphJNITest.java")
set(BW_JAVA_BENCH "${PROJECT_SOURCE_DIR}/bindings/java/test/BwGraphJNIBench.java")
set(BW_JAVA_DIR "${PROJECT_BINARY_DIR}/java")
set(BW_JNI_HEADER "${BW_JAVA_DIR}/include/db_monacgraph_jni_BwGraphJNI.h")
add_custom_command(OUTPUT "${BW_JNI_HEADER}" "${BW_JAVA_DIR}/bwgraph-jni.jar"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${BW_JAVA_DIR}/include" "${BW_JAVA_DIR}/classes"
  COMMAND ${Java_JAVAC_EXECUTABLE} --release 17 -encoding UTF-8
    -h "${BW_JAVA_DIR}/include" -d "${BW_JAVA_DIR}/classes"
    "${BW_JAVA_SOURCE}" "${BW_JAVA_TEST}" "${BW_JAVA_BENCH}"
  COMMAND ${Java_JAR_EXECUTABLE} --create --file "${BW_JAVA_DIR}/bwgraph-jni.jar"
    -C "${BW_JAVA_DIR}/classes" db
  DEPENDS "${BW_JAVA_SOURCE}" "${BW_JAVA_TEST}" "${BW_JAVA_BENCH}" VERBATIM)
add_custom_target(bwgraph_java ALL DEPENDS "${BW_JNI_HEADER}" "${BW_JAVA_DIR}/bwgraph-jni.jar")
set_target_properties(bwgraph PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(bwgraph_jni SHARED "${PROJECT_SOURCE_DIR}/bindings/jni/bw_graph_jni.cc" "${BW_JNI_HEADER}")
add_dependencies(bwgraph_jni bwgraph_java)
target_include_directories(bwgraph_jni PRIVATE ${JNI_INCLUDE_DIRS} "${BW_JAVA_DIR}/include")
target_link_libraries(bwgraph_jni PRIVATE bwgraph)
set_target_properties(bwgraph_jni PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
find_program(BW_BASH_EXECUTABLE bash REQUIRED)
add_test(NAME test_jni COMMAND ${BW_BASH_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/jni_java.sh"
  ${Java_JAVA_EXECUTABLE} -Xcheck:jni -ea
  "-Dbwgraph.jni.path=$<TARGET_FILE:bwgraph_jni>" -cp "${BW_JAVA_DIR}/classes" BwGraphJNITest)
set_tests_properties(test_jni PROPERTIES TIMEOUT 180)
