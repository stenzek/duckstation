# SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0 + Packaging Restriction
#
# NOTE: In addition to the terms of CC-BY-NC-ND-4.0, you may not use this file to create
# packages or build recipes without explicit permission from the copyright holder.

add_library(util
  animated_image.cpp
  animated_image.h
  audio_stream.cpp
  audio_stream.h
  cd_image.cpp
  cd_image.h
  cd_image_ccd.cpp
  cd_image_cue.cpp
  cd_image_chd.cpp
  cd_image_device.cpp
  cd_image_hasher.cpp
  cd_image_hasher.h
  cd_image_m3u.cpp
  cd_image_memory.cpp
  cd_image_mds.cpp
  cd_image_pbp.cpp
  cd_image_ppf.cpp
  compress_helpers.cpp
  compress_helpers.h
  core_audio_stream.cpp
  core_audio_stream.h
  cubeb_audio_stream.cpp
  cue_parser.cpp
  cue_parser.h
  dyn_libjpeg.h
  dyn_libpng.h
  dyn_libs.cpp
  dyn_libwebp.h
  dyn_libzip.h
  dyn_sdl.h
  dyn_shaderc.h
  dyn_spirv_cross.h
  dyn_sqlite.h
  elf_file.cpp
  elf_file.h
  gpu_device.cpp
  gpu_device.h
  gpu_framebuffer_manager.h
  gpu_texture.cpp
  gpu_texture.h
  gpu_types.h
  http_cache.cpp
  http_cache.h
  http_downloader.cpp
  http_downloader.h
  image.cpp
  image.h
  imgui_gsvector.h
  imgui_manager.cpp
  imgui_manager.h
  ini_settings_interface.cpp
  ini_settings_interface.h
  input_manager.cpp
  input_manager.h
  input_manager_private.h
  input_source.cpp
  input_source.h
  iso_reader.cpp
  iso_reader.h
  media_capture.cpp
  media_capture.h
  object_archive.cpp
  object_archive.h
  page_fault_handler.cpp
  page_fault_handler.h
  postprocessing.cpp
  postprocessing.h
  postprocessing_shader.cpp
  postprocessing_shader.h
  postprocessing_shader_fx.cpp
  postprocessing_shader_fx.h
  postprocessing_shader_glsl.cpp
  postprocessing_shader_glsl.h
  postprocessing_shader_slang.cpp
  postprocessing_shader_slang.h
  sdl_audio_stream.cpp
  sdl_input_source.cpp
  sdl_input_source.h
  shadergen.cpp
  shadergen.h
  shiftjis.cpp
  shiftjis.h
  sockets.cpp
  sockets.h
  spirv_module.cpp
  spirv_module.h
  sqlite_helpers.cpp
  sqlite_helpers.h
  state_wrapper.cpp
  state_wrapper.h
  texture_decompress.cpp
  texture_decompress.h
  translation.cpp
  translation.h
  wav_reader_writer.cpp
  wav_reader_writer.h
  window_info.cpp
  window_info.h
  zip_helpers.cpp
  zip_helpers.h
)

target_precompile_headers(util PRIVATE "pch.h")
target_include_directories(util PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
get_target_property(JPEG_INCLUDE_DIRS libjpeg-turbo::jpeg INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(PNG_INCLUDE_DIRS PNG::png_shared INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(WEBP_INCLUDE_DIRS WebP::webp INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(LIBZIP_INCLUDE_DIRS libzip::zip INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(util PRIVATE ${JPEG_INCLUDE_DIRS} ${PNG_INCLUDE_DIRS} ${WEBP_INCLUDE_DIRS} ${LIBZIP_INCLUDE_DIRS})
target_link_libraries(util PUBLIC common imgui)
target_link_libraries(util PRIVATE libchdr lzma reshadefx cubeb xxhash)
target_link_libraries(util PRIVATE CURL::libcurl SDL3::Headers SoundTouch::SoundTouchDLL ZLIB::ZLIB zstd::libzstd_shared)

if(ENABLE_X11)
  target_sources(util PRIVATE
    x11_tools.cpp
    x11_tools.h
  )
  target_compile_definitions(util PRIVATE "-DENABLE_X11=1")
  target_include_directories(util PRIVATE "${X11_xcb_INCLUDE_PATH}")
endif()

if(ENABLE_WAYLAND)
  target_compile_definitions(util PRIVATE "-DENABLE_WAYLAND=1")
endif()

if(ENABLE_OPENGL)
  target_sources(util PRIVATE
    opengl_context.cpp
    opengl_context.h
    opengl_device.cpp
    opengl_device.h
    opengl_loader.h
    opengl_pipeline.cpp
    opengl_pipeline.h
    opengl_stream_buffer.cpp
    opengl_stream_buffer.h
    opengl_texture.cpp
    opengl_texture.h
  )
  target_compile_definitions(util PUBLIC "ENABLE_OPENGL=1")
  target_link_libraries(util PRIVATE glad)

  if(LINUX OR BSD)
    target_sources(util PRIVATE
      opengl_context_egl.cpp
      opengl_context_egl.h
    )
    target_compile_definitions(util PRIVATE "-DENABLE_EGL=1")

    if(ENABLE_X11)
      target_sources(util PRIVATE
        opengl_context_egl_xcb.cpp
        opengl_context_egl_xcb.h
        opengl_context_egl_xlib.cpp
        opengl_context_egl_xlib.h
      )
    endif()
    if(ENABLE_WAYLAND)
      target_sources(util PRIVATE
        opengl_context_egl_wayland.cpp
        opengl_context_egl_wayland.h
      )
      target_include_directories(util PRIVATE
        "${Wayland_INCLUDE_DIRS}" # For wayland-egl
      )
    endif()
  endif()

  if(APPLE)
    target_sources(util PRIVATE
      opengl_context_agl.mm
      opengl_context_agl.h
    )
    set_source_files_properties(opengl_context_agl.mm PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)
  endif()
endif()

if(ENABLE_VULKAN)
  target_sources(util PRIVATE
    vulkan_builders.cpp
    vulkan_builders.h
    vulkan_device.cpp
    vulkan_device.h
    vulkan_entry_points.inl
    vulkan_headers.h
    vulkan_loader.cpp
    vulkan_loader.h
    vulkan_pipeline.cpp
    vulkan_pipeline.h
    vulkan_stream_buffer.cpp
    vulkan_stream_buffer.h
    vulkan_swap_chain.cpp
    vulkan_swap_chain.h
    vulkan_texture.cpp
    vulkan_texture.h
  )
  target_compile_definitions(util PUBLIC "ENABLE_VULKAN=1")
  target_link_libraries(util PUBLIC vulkan-headers)
endif()

# freetype/plutosvg/shaderc/spirv-cross/ffmpeg is loaded dynamically to reduce module loads on startup.
get_target_property(FREETYPE_INCLUDE_DIRS freetype INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(PLUTOSVG_INCLUDE_DIRS plutosvg::plutosvg INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(SHADERC_INCLUDE_DIR Shaderc::shaderc_shared INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(SPIRV_CROSS_INCLUDE_DIR spirv-cross-c-shared INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(util PUBLIC
  ${FREETYPE_INCLUDE_DIRS}
  ${PLUTOSVG_INCLUDE_DIRS}
  ${SHADERC_INCLUDE_DIR}
  ${SPIRV_CROSS_INCLUDE_DIR}
  ${FFMPEG_INCLUDE_DIRS}
)

if(APPLE)
  set(MAC_SOURCES
    metal_device.h
    metal_device.mm
    metal_stream_buffer.h
    metal_stream_buffer.mm
  )
  set(METAL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/metal_shaders.metal"
  )
  set_property(GLOBAL PROPERTY UTIL_METAL_SOURCES ${METAL_SOURCES})
  target_sources(util PRIVATE ${MAC_SOURCES})
  find_library(IOK_LIBRARY IOKit REQUIRED)
  find_library(METAL_LIBRARY Metal)
  find_library(QUARTZCORE_LIBRARY QuartzCore)
  target_link_libraries(util PRIVATE ${METAL_LIBRARY} ${QUARTZCORE_LIBRARY} ${IOK_LIBRARY})
  set_source_files_properties(${MAC_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)
elseif(LINUX)
  target_link_libraries(util PRIVATE UDEV::UDEV)
endif()

function(add_util_resources target)
  if(APPLE)
    get_property(UTIL_METAL_SOURCES GLOBAL PROPERTY UTIL_METAL_SOURCES)
    add_metal_sources(${target} ${UTIL_METAL_SOURCES} metal_shaders macos-metal2.3)

    if(BUILD_MACOS_BUNDLE)
      # Copy MoltenVK into the bundle
      unset(MOLTENVK_PATH CACHE)
      find_file(MOLTENVK_PATH NAMES
        libMoltenVK.dylib
        lib/libMoltenVK.dylib
      )
      if (MOLTENVK_PATH)
        target_sources(${target} PRIVATE "${MOLTENVK_PATH}")
        set_source_files_properties("${MOLTENVK_PATH}" PROPERTIES MACOSX_PACKAGE_LOCATION Frameworks)
        message(STATUS "Using MoltenVK from ${MOLTENVK_PATH}")
      else()
        message(WARNING "MoltenVK not found in path, it will depend on the target system having it.")
      endif()

      # Copy ffmpeg into the bundle.
      foreach(component avcodec avformat avutil swresample swscale)
        string(REGEX REPLACE "\([0-9]+\)\.[0-9]+\.[0-9]+" "\\1" major "${FFMPEG_${component}_VERSION}")
        string(REPLACE "lib${component}.dylib" "lib${component}.${major}.dylib" version_lib "${FFMPEG_${component}_LIBRARIES}")
        target_sources(${target} PRIVATE ${version_lib})
        set_source_files_properties(${target} PRIVATE ${version_lib} PROPERTIES MACOSX_PACKAGE_LOCATION Frameworks)
      endforeach()
    endif()
  endif()

  # Copy dynamically-loaded libraries into the bundle.
  add_runtime_libraries(${target} spirv-cross-c-shared Shaderc::shaderc_shared SQLite3::sqlite3-shared SDL3::SDL3
                        freetype harfbuzz::harfbuzz plutosvg::plutosvg libjpeg-turbo::jpeg PNG::png_shared libzip::zip
                        WebP::webp WebP::sharpyuv WebP::webpdecoder WebP::webpdemux WebP::libwebpmux)
endfunction()
