set(SRCS
  include/kiss_fft.h
  include/kiss_fftr.h
  src/_kiss_fft_guts.h
  src/kiss_fft.cpp
  src/kiss_fftr.cpp
)

add_library(kissfft ${SRCS})
target_include_directories(kissfft PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(kissfft INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

