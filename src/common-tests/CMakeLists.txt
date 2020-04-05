add_executable(common-tests
  event_tests.cpp
  rectangle_tests.cpp
)

target_link_libraries(common-tests PRIVATE common gtest gtest_main)
