set(SRCS
  include/stb_image.h
  include/stb_image_resize.h
  include/stb_image_write.h
  src/stb_image.c
  src/stb_image_resize.c
  src/stb_image_write.c
)

add_library(stb ${SRCS})
target_include_directories(stb PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(stb INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(stb zlib Threads::Threads "${CMAKE_DL_LIBS}")
