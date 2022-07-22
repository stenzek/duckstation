add_library(util
  audio_stream.cpp
  audio_stream.h
  cd_image.cpp
  cd_image.h
  cd_image_bin.cpp
  cd_image_cue.cpp
  cd_image_chd.cpp
  cd_image_device.cpp
  cd_image_ecm.cpp
  cd_image_hasher.cpp
  cd_image_hasher.h
  cd_image_m3u.cpp
  cd_image_memory.cpp
  cd_image_mds.cpp
  cd_image_pbp.cpp
  cd_image_ppf.cpp
  cd_subchannel_replacement.cpp
  cd_subchannel_replacement.h
  cd_xa.cpp
  cd_xa.h
  cue_parser.cpp
  cue_parser.h
  ini_settings_interface.cpp
  ini_settings_interface.h
  iso_reader.cpp
  iso_reader.h
  jit_code_buffer.cpp
  jit_code_buffer.h
  null_audio_stream.cpp
  null_audio_stream.h
  memory_arena.cpp
  memory_arena.h
  page_fault_handler.cpp
  page_fault_handler.h
  shiftjis.cpp
  shiftjis.h
  state_wrapper.cpp
  state_wrapper.h
  wav_writer.cpp
  wav_writer.h
)

target_include_directories(util PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(util PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(util PUBLIC common simpleini)
target_link_libraries(util PRIVATE libchdr samplerate zlib)
