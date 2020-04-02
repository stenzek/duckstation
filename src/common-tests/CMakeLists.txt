add_executable(common-tests
  rectangle_tests.cpp
)

target_link_libraries(common-tests PRIVATE common gtest gtest_main)
