set(SRCS
  include/tinyxml2.h
  src/tinyxml2.cpp
)

add_library(tinyxml2 ${SRCS})
target_include_directories(tinyxml2 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

