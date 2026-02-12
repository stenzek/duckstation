add_executable(util-tests
  animated_image_tests.cpp
  elf_parser_tests.cpp
  cue_parser_tests.cpp
  image_tests.cpp
  stub_host.cpp
)

target_include_directories(util-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(util-tests PRIVATE util gtest gtest_main)
