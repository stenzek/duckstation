set(SRCS
  include/xxh3.h
  include/xxhash.h
  src/xxhash.c
)

add_library(xxhash ${SRCS})
target_include_directories(xxhash PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(xxhash INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(xxhash INTERFACE "XXH_STATIC_LINKING_ONLY")

