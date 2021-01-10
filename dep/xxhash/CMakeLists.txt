set(SRCS
  include/xxhash.h
  src/xxhash.c
)

add_library(xxhash ${SRCS})

if(${CPU_ARCH} STREQUAL "x86" OR ${CPU_ARCH} STREQUAL "x64")
  # Required if building with -mavx or -march=native, but you shouldn't be building with it.
  target_compile_definitions(xxhash PRIVATE "XXH_X86DISPATCH_ALLOW_AVX")
  target_sources(xxhash PRIVATE
    include/xxh_x86dispatch.h
    src/xxh_x86dispatch.c
  )
endif()

target_include_directories(xxhash PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(xxhash INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(xxhash INTERFACE "XXH_STATIC_LINKING_ONLY")

