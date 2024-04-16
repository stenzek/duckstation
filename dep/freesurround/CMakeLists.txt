set(SRCS
  include/freesurround_decoder.h
  src/channelmaps.cpp
  src/freesurround_decoder.cpp
)

add_library(freesurround ${SRCS})
target_include_directories(freesurround PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(freesurround INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(freesurround PUBLIC kissfft)
