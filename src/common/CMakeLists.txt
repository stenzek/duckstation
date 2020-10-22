add_library(common
  align.h
  assert.cpp
  assert.h
  audio_stream.cpp
  audio_stream.h
  bitfield.h
  bitutils.h
  byte_stream.cpp
  byte_stream.h
  cd_image.cpp
  cd_image.h
  cd_image_bin.cpp
  cd_image_cue.cpp
  cd_image_chd.cpp
  cd_image_hasher.cpp
  cd_image_hasher.h
  cd_image_memory.cpp
  cd_subchannel_replacement.cpp
  cd_subchannel_replacement.h
  cd_xa.cpp
  cd_xa.h
  cpu_detect.h
  dimensional_array.h
  event.cpp
  event.h
  fifo_queue.h
  file_system.cpp
  file_system.h
  image.cpp
  image.h
  gl/context.cpp
  gl/context.h
  gl/program.cpp
  gl/program.h
  gl/shader_cache.cpp
  gl/shader_cache.h
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
  make_array.h
  md5_digest.cpp
  md5_digest.h
  minizip_helpers.cpp
  minizip_helpers.h
  null_audio_stream.cpp
  null_audio_stream.h
  memory_arena.cpp
  memory_arena.h
  page_fault_handler.cpp
  page_fault_handler.h
  rectangle.h
  progress_callback.cpp
  progress_callback.h
  scope_guard.h
  shiftjis.cpp
  shiftjis.h
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
  vulkan/builders.cpp
  vulkan/builders.h
  vulkan/context.cpp
  vulkan/context.h
  vulkan/shader_cache.cpp
  vulkan/shader_cache.h
  vulkan/shader_compiler.cpp
  vulkan/shader_compiler.h
  vulkan/staging_buffer.cpp
  vulkan/staging_buffer.h
  vulkan/staging_texture.cpp
  vulkan/staging_texture.h
  vulkan/stream_buffer.cpp
  vulkan/stream_buffer.h
  vulkan/swap_chain.cpp
  vulkan/swap_chain.h
  vulkan/texture.cpp
  vulkan/texture.h
  vulkan/util.cpp
  vulkan/util.h
  wav_writer.cpp
  wav_writer.h
)

target_include_directories(common PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(common PRIVATE glad libcue stb Threads::Threads libchdr glslang vulkan-loader zlib minizip)

if(WIN32)
  target_sources(common PRIVATE
    gl/context_wgl.cpp
    gl/context_wgl.h
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
    win32_progress_callback.cpp
    win32_progress_callback.h
  )
  target_link_libraries(common PRIVATE d3dcompiler.lib)
endif()

if(ANDROID)
  target_link_libraries(common PRIVATE log)
endif()

if(USE_X11)
  target_sources(common PRIVATE
    gl/x11_window.cpp
    gl/x11_window.h
  )
  target_compile_definitions(common PRIVATE "-DUSE_X11=1")
  target_include_directories(common PRIVATE "${X11_INCLUDE_DIR}")
  target_link_libraries(common PRIVATE "${X11_LIBRARIES}")
endif()

if(USE_EGL)
  target_sources(common PRIVATE
    gl/context_egl.cpp
    gl/context_egl.h
  )
  target_compile_definitions(common PRIVATE "-DUSE_EGL=1")

  if(USE_X11)
    target_sources(common PRIVATE
      gl/context_egl_x11.cpp
      gl/context_egl_x11.h
    )
  endif()
  if(ANDROID AND USE_EGL)
    target_sources(common PRIVATE
      gl/context_egl_android.cpp
      gl/context_egl_android.h
    )
  endif()
endif()

if(USE_X11)
  target_sources(common PRIVATE
    gl/context_glx.cpp
    gl/context_glx.h
  )
  target_compile_definitions(common PRIVATE "-DUSE_GLX=1")
endif()

if(USE_WAYLAND)
  target_sources(common PRIVATE
    gl/context_egl_wayland.cpp
    gl/context_egl_wayland.h
  )
  target_compile_definitions(common PRIVATE "-DUSE_WAYLAND=1")
  target_link_libraries(common PRIVATE Wayland::Egl)
endif()

if(APPLE)
  # Needed for Vulkan Swap Chain.
  target_link_libraries(common PRIVATE "objc")
endif()

if(APPLE AND NOT BUILD_LIBRETRO_CORE)
  target_sources(common PRIVATE
    gl/context_agl.mm
    gl/context_agl.h
  )
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  # We need -lrt for shm_unlink
  target_link_libraries(common PRIVATE rt)
endif()
