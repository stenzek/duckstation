add_library(common
  align.h
  assert.cpp
  assert.h
  audio_stream.cpp
  audio_stream.h
  bitfield.h
  byte_stream.cpp
  byte_stream.h
  cd_image.cpp
  cd_image.h
  cd_image_bin.cpp
  cd_image_cue.cpp
  cd_image_chd.cpp
  cd_subchannel_replacement.cpp
  cd_subchannel_replacement.h
  cd_xa.cpp
  cd_xa.h
  cpu_detect.h
  cubeb_audio_stream.cpp
  cubeb_audio_stream.h
  fifo_queue.h
  file_system.cpp
  file_system.h
  gl/program.cpp
  gl/program.h
  gl/stream_buffer.cpp
  gl/stream_buffer.h
  gl/texture.cpp
  gl/texture.h
  hash_combine.h
  heap_array.h
  iso_reader.cpp
  iso_reader.h
  jit_code_buffer.cpp
  jit_code_buffer.h
  log.cpp
  log.h
  md5_digest.cpp
  md5_digest.h
  null_audio_stream.cpp
  null_audio_stream.h
  rectangle.h
  progress_callback.cpp
  progress_callback.h
  state_wrapper.cpp
  state_wrapper.h
  string.cpp
  string.h
  string_util.cpp
  string_util.h
  timer.cpp
  timer.h
  timestamp.cpp
  timestamp.h
  types.h
)

target_include_directories(common PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(common PRIVATE glad libcue Threads::Threads cubeb libchdr)

if(WIN32)
  target_sources(common PRIVATE
    d3d11/shader_cache.cpp
    d3d11/shader_cache.h
    d3d11/shader_compiler.cpp
    d3d11/shader_compiler.h
    d3d11/staging_texture.cpp
    d3d11/staging_texture.h
    d3d11/stream_buffer.cpp
    d3d11/stream_buffer.h
    d3d11/texture.cpp
    d3d11/texture.h
    windows_headers.h
  )
  target_link_libraries(common PRIVATE d3dcompiler.lib)
endif()

if(ANDROID)
  target_link_libraries(common PRIVATE log)
endif()
