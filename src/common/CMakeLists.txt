add_library(common
  align.h
  assert.cpp
  assert.h
  bitfield.h
  bitutils.h
  build_timestamp.h
  byte_stream.cpp
  byte_stream.h
  crash_handler.cpp
  crash_handler.h
  dimensional_array.h
  error.cpp
  error.h
  fastjmp.cpp
  fastjmp.h
  fifo_queue.h
  file_system.cpp
  file_system.h
  image.cpp
  image.h
  hash_combine.h
  heap_array.h
  heterogeneous_containers.h
  http_downloader.cpp
  http_downloader.h
  layered_settings_interface.cpp
  layered_settings_interface.h
  log.cpp
  log.h
  make_array.h
  md5_digest.cpp
  md5_digest.h
  memory_settings_interface.cpp
  memory_settings_interface.h
  minizip_helpers.cpp
  minizip_helpers.h
  path.h
  platform.h
  progress_callback.cpp
  progress_callback.h
  rectangle.h
  scoped_guard.h
  settings_interface.h
  sha1_digest.cpp
  sha1_digest.h
  string.cpp
  string.h
  string_util.cpp
  string_util.h
  thirdparty/thread_pool.cpp
  thirdparty/thread_pool.h
  threading.cpp
  threading.h
  timer.cpp
  timer.h
  types.h
)

target_include_directories(common PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(common PUBLIC fmt Threads::Threads vulkan-headers GSL fast_float)
target_link_libraries(common PRIVATE stb libchdr zlib minizip Zstd::Zstd "${CMAKE_DL_LIBS}")

if(WIN32)
  target_sources(common PRIVATE
    http_downloader_winhttp.cpp
    http_downloader_winhttp.h
    thirdparty/StackWalker.cpp
    thirdparty/StackWalker.h
    win32_progress_callback.cpp
    win32_progress_callback.h
    windows_headers.h
  )
  target_link_libraries(common PRIVATE d3dcompiler.lib)

  if(${CPU_ARCH} STREQUAL "x64")
    enable_language(ASM_MASM)
    target_sources(common PRIVATE fastjmp_x86.asm)
  elseif(${CPU_ARCH} STREQUAL "aarch32" OR ${CPU_ARCH} STREQUAL "aarch64")
    enable_language(ASM_MARMASM)
    target_sources(common PRIVATE fastjmp_arm.asm)
  endif()
endif()

if(NOT WIN32 AND NOT ANDROID)
  target_sources(common PRIVATE
    http_downloader_curl.cpp
    http_downloader_curl.h
  )
  target_link_libraries(common PRIVATE
    CURL::libcurl
  )
endif()

if(ANDROID)
  target_link_libraries(common PRIVATE log)
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  # We need -lrt for shm_unlink
  target_link_libraries(common PRIVATE rt)
endif()
