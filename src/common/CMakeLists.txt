add_library(common
  audio_stream.cpp
  audio_stream.h
  bitfield.h
  cd_image.cpp
  cd_image.h
  cd_image_bin.cpp
  cd_image_cue.cpp
  cd_subchannel_replacement.cpp
  cd_subchannel_replacement.h
  cd_xa.cpp
  cd_xa.h
  gl/program.cpp
  gl/program.h
  gl/stream_buffer.cpp
  gl/stream_buffer.h
  gl/texture.cpp
  gl/texture.h
  iso_reader.cpp
  iso_reader.h
  jit_code_buffer.cpp
  jit_code_buffer.h
  null_audio_stream.cpp
  null_audio_stream.h
  rectangle.h
  state_wrapper.cpp
  state_wrapper.h
  types.h
)

target_include_directories(common PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(common PRIVATE YBaseLib glad libcue Threads::Threads)

if(WIN32)
  target_sources(common PRIVATE
    d3d11/shader_compiler.cpp
    d3d11/shader_compiler.h
    d3d11/staging_texture.cpp
    d3d11/staging_texture.h
    d3d11/stream_buffer.cpp
    d3d11/stream_buffer.h
    d3d11/texture.cpp
    d3d11/texture.h
  )
  target_link_libraries(common PRIVATE d3dcompiler.lib)
endif()
