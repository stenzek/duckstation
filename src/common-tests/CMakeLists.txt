add_executable(common-tests
  binary_reader_writer_tests.cpp
  bitutils_tests.cpp
  file_system_tests.cpp
  gsvector_tests.cpp
  gsvector_yuvtorgb_test.cpp
  hash_tests.cpp
  heap_array_tests.cpp
  path_tests.cpp
  rectangle_tests.cpp
  small_string_tests.cpp
  string_tests.cpp
)

target_include_directories(common-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(common-tests PRIVATE common gtest gtest_main)
