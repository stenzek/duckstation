add_library(libchdr
  include/libchdr/bitstream.h
  include/libchdr/cdrom.h
  include/libchdr/chd.h
  include/libchdr/coretypes.h
  include/libchdr/flac.h
  include/libchdr/huffman.h
  src/bitstream.c
  src/cdrom.c
  src/chd.c
  src/flac.c
  src/huffman.c
  src/md5.c
  src/md5.h
  src/sha1.c
  src/sha1.h
)

target_include_directories(libchdr PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/libchdr")
target_include_directories(libchdr INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(libchdr PRIVATE zlib lzma libFLAC)

