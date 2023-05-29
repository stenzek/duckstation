cmake_minimum_required(VERSION 3.15)
project(biscuit VERSION 0.9.1)

#include(CTest)

option(BISCUIT_CODE_BUFFER_MMAP "Use mmap for handling code buffers instead of new" OFF)

# Source directories
add_subdirectory(src)

#if (BUILD_TESTING)
#    add_subdirectory(tests)
#endif()

#if (BUILD_EXAMPLES)
#    add_subdirectory(examples)
#endif()
