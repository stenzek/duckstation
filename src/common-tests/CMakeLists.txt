add_executable(common-tests
  bitutils_tests.cpp
  file_system_tests.cpp
  gsvector_yuvtorgb_test.cpp
  path_tests.cpp
  rectangle_tests.cpp
  string_tests.cpp
)

target_link_libraries(common-tests PRIVATE common gtest gtest_main)
