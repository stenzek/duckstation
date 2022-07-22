add_library(common
  align.h
  assert.cpp
  assert.h
  bitfield.h
  bitutils.h
  byte_stream.cpp
  byte_stream.h
  crash_handler.cpp
  crash_handler.h
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
  heterogeneous_containers.h
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
  scope_guard.h
  settings_interface.h
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
  vulkan/builders.cpp
  vulkan/builders.h
  vulkan/context.cpp
  vulkan/context.h
  vulkan/loader.h
  vulkan/loader.cpp
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
  window_info.cpp
  window_info.h
)

target_include_directories(common PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(common PUBLIC fmt Threads::Threads)
target_link_libraries(common PRIVATE glad stb libchdr glslang zlib minizip samplerate "${CMAKE_DL_LIBS}")

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
    gl/context_wgl.cpp
    gl/context_wgl.h
    thirdparty/StackWalker.cpp
    thirdparty/StackWalker.h
    win32_progress_callback.cpp
    win32_progress_callback.h
    windows_headers.h
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
  target_sources(common PRIVATE
    gl/context_agl.mm
    gl/context_agl.h
  )
endif()

if(ENABLE_CHEEVOS)
  target_sources(common PRIVATE
    http_downloader.cpp
    http_downloader.h
  )
  if(WIN32)
    target_sources(common PRIVATE
      http_downloader_winhttp.cpp
      http_downloader_winhttp.h
    )
  elseif(NOT ANDROID)
    target_sources(common PRIVATE
      http_downloader_curl.cpp
      http_downloader_curl.h
    )
    target_link_libraries(common PRIVATE
      CURL::libcurl
    )
  endif()
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  # We need -lrt for shm_unlink
  target_link_libraries(common PRIVATE rt)
endif()
