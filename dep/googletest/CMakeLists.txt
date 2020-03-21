set(SRCS
    src/gtest.cc
    src/gtest-death-test.cc
    src/gtest-filepath.cc
    src/gtest-matchers.cc
    src/gtest-port.cc
    src/gtest-printers.cc
    src/gtest-test-part.cc
    src/gtest-typed-test.cc
)

set(MAIN_SRCS
    src/gtest_main.cc
)

add_library(gtest ${SRCS})
target_include_directories(gtest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(gtest INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gtest Threads::Threads)

add_library(gtest_main "${MAIN_SRCS}")
target_include_directories(gtest_main PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(gtest_main INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gtest_main Threads::Threads)

