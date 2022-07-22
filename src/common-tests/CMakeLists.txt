add_executable(common-tests
  bitutils_tests.cpp
  event_tests.cpp
  file_system_tests.cpp
  path_tests.cpp
  rectangle_tests.cpp
)

target_link_libraries(common-tests PRIVATE common gtest gtest_main)
