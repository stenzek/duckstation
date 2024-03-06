add_library(zlib
  include/zconf.h
  include/zlib.h
  src/adler32.c
  src/compress.c
  src/crc32.c
  src/crc32.h
  src/deflate.c
  src/deflate.h
  src/gzclose.c
  src/gzguts.h
  src/gzlib.c
  src/gzread.c
  src/gzwrite.c
  src/infback.c
  src/inffast.c
  src/inffast.h
  src/inffixed.h
  src/inflate.c
  src/inflate.h
  src/inftrees.c
  src/inftrees.h
  src/trees.c
  src/trees.h
  src/uncompr.c
  src/zutil.c
  src/zutil.h
)

target_include_directories(zlib PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(zlib INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
disable_compiler_warnings_for_target(zlib)
add_library(ZLIB::ZLIB ALIAS zlib)
