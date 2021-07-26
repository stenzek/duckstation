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
  crash_handler.cpp
  crash_handler.h
  cue_parser.cpp
  cue_parser.h
  dimensional_array.h
  error.cpp
  error.h
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
  platform.h
  pbp_types.h
  progress_callback.cpp
  progress_callback.h
  rectangle.h
  scope_guard.h
  shiftjis.cpp
  shiftjis.h
  state_wrapper.cpp
  state_wrapper.h
  string.cpp
  string.h
  string_util.cpp
  string_util.h
  thirdparty/thread_pool.cpp
  thirdparty/thread_pool.h
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
  window_info.cpp
  window_info.h
)

target_include_directories(common PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(common PRIVATE glad stb Threads::Threads libchdr glslang vulkan-loader zlib minizip samplerate)

if(WIN32)
  target_sources(common PRIVATE
    d3d12/context.cpp
    d3d12/context.h
    d3d12/descriptor_heap_manager.cpp
    d3d12/descriptor_heap_manager.h
    d3d12/shader_cache.cpp
    d3d12/shader_cache.h
    d3d12/staging_texture.cpp
    d3d12/staging_texture.h
    d3d12/stream_buffer.cpp
    d3d12/stream_buffer.h
    d3d12/texture.cpp
    d3d12/texture.h
    d3d12/util.cpp
    d3d12/util.h
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

  if(NOT LIBRETRO)
    target_sources(common PRIVATE
      gl/context_wgl.cpp
      gl/context_wgl.h
      thirdparty/StackWalker.cpp
      thirdparty/StackWalker.h
      win32_progress_callback.cpp
      win32_progress_callback.h
    )
  endif()
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
  target_include_directories(common PRIVATE "${X11_INCLUDE_DIR}" "${X11_Xrandr_INCLUDE_PATH}")
  target_link_libraries(common PRIVATE "${X11_LIBRARIES}" "${X11_Xrandr_LIB}")
endif()

if(USE_DRMKMS)
  target_sources(common PRIVATE
    drm_display.cpp
    drm_display.h
  )
  target_link_libraries(common PUBLIC Libdrm::Libdrm)
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

    # We set EGL_NO_X11 because otherwise X comes in with its macros and breaks
    # a bunch of files from compiling, if we include the EGL headers. This just
    # makes the data types opaque, we can still use it with X11 if needed.
    target_compile_definitions(common PRIVATE "-DEGL_NO_X11=1")
  endif()
  if(ANDROID AND USE_EGL)
    target_sources(common PRIVATE
      gl/context_egl_android.cpp
      gl/context_egl_android.h
    )
  endif()
  if(USE_DRMKMS)
    target_compile_definitions(common PRIVATE "-DUSE_GBM=1")
    target_sources(common PRIVATE
      gl/context_egl_gbm.cpp
      gl/context_egl_gbm.h
    )
    target_link_libraries(common PUBLIC GBM::GBM)
  endif()
  if(USE_FBDEV)
    target_compile_definitions(common PRIVATE "-DUSE_FBDEV=1")
    target_sources(common PRIVATE
      gl/context_egl_fbdev.cpp
      gl/context_egl_fbdev.h
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
