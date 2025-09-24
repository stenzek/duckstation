add_executable(util-tests
  image_tests.cpp
)

target_link_libraries(util-tests PRIVATE util gtest gtest_main)
