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
  cue_parser.cpp
  cue_parser.h
  gpu_device.cpp
  gpu_device.h
  gpu_framebuffer_manager.h
  gpu_shader_cache.cpp
  gpu_shader_cache.h
  gpu_texture.cpp
  gpu_texture.h
  host.cpp
  host.h
  http_downloader.cpp
  http_downloader.h
  image.cpp
  image.h
  imgui_fullscreen.cpp
  imgui_fullscreen.h
  imgui_manager.cpp
  imgui_manager.h
  ini_settings_interface.cpp
  ini_settings_interface.h
  input_manager.cpp
  input_manager.h
  input_source.cpp
  input_source.h
  iso_reader.cpp
  iso_reader.h
  media_capture.cpp
  media_capture.h
  page_fault_handler.cpp
  page_fault_handler.h
  platform_misc.h
  postprocessing.cpp
  postprocessing.h
  postprocessing_shader.cpp
  postprocessing_shader.h
  postprocessing_shader_fx.cpp
  postprocessing_shader_fx.h
  postprocessing_shader_glsl.cpp
  postprocessing_shader_glsl.h
  shadergen.cpp
  shadergen.h
  shiftjis.cpp
  shiftjis.h
  sockets.cpp
  sockets.h
  state_wrapper.cpp
  state_wrapper.h
  wav_writer.cpp
  wav_writer.h
  window_info.cpp
  window_info.h
)

target_precompile_headers(util PRIVATE "pch.h")
target_include_directories(util PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(util PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(util PUBLIC common simpleini imgui)
target_link_libraries(util PRIVATE libchdr JPEG::JPEG PNG::PNG WebP::libwebp ZLIB::ZLIB SoundTouch::SoundTouchDLL xxhash Zstd::Zstd reshadefx)

if(ENABLE_X11)
  target_compile_definitions(util PRIVATE "-DENABLE_X11=1")
  target_link_libraries(util PRIVATE X11::X11 X11::Xrandr)
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

  if(WIN32)
    target_sources(util PRIVATE
      opengl_context_wgl.cpp
      opengl_context_wgl.h
    )
    target_link_libraries(util PRIVATE "opengl32.lib")
  endif()

  if(LINUX OR BSD OR ANDROID)
    target_sources(util PRIVATE
      opengl_context_egl.cpp
      opengl_context_egl.h
    )
    target_compile_definitions(util PRIVATE "-DENABLE_EGL=1")

    if(ENABLE_X11)
      target_sources(util PRIVATE
        opengl_context_egl_x11.cpp
        opengl_context_egl_x11.h
      )
    endif()
    if(ENABLE_WAYLAND)
      target_sources(util PRIVATE
        opengl_context_egl_wayland.cpp
        opengl_context_egl_wayland.h
      )
    endif()
    if(ANDROID)
      target_include_directories(util PRIVATE "${CMAKE_SOURCE_DIR}/android/app/src/cpp")
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
    vulkan_entry_points.h
    vulkan_entry_points.inl
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

if(ENABLE_VULKAN OR APPLE)
  # shaderc is loaded dynamically to reduce module loads on startup.
  get_target_property(SHADERC_INCLUDE_DIR Shaderc::shaderc_shared INTERFACE_INCLUDE_DIRECTORIES)
  target_include_directories(util PUBLIC ${SHADERC_INCLUDE_DIR})
endif()

if(NOT ANDROID)
  target_sources(util PRIVATE
    cubeb_audio_stream.cpp
    sdl_audio_stream.cpp
    sdl_input_source.cpp
    sdl_input_source.h
  )
  target_link_libraries(util PUBLIC
    cubeb
    freesurround
    SDL2::SDL2
  )
  # FFmpeg loaded dynamically on demand.
  target_include_directories(util PUBLIC ${FFMPEG_INCLUDE_DIRS})
endif()

if(WIN32)
  target_sources(util PRIVATE
    d3d_common.cpp
    d3d_common.h
    d3d11_device.cpp
    d3d11_device.h
    d3d11_pipeline.cpp
    d3d11_pipeline.h
    d3d11_stream_buffer.cpp
    d3d11_stream_buffer.h
    d3d11_texture.cpp
    d3d11_texture.h
    d3d12_builders.cpp
    d3d12_builders.h
    d3d12_descriptor_heap_manager.cpp
    d3d12_descriptor_heap_manager.h
    d3d12_device.cpp
    d3d12_device.h
    d3d12_pipeline.cpp
    d3d12_pipeline.h
    d3d12_stream_buffer.cpp
    d3d12_stream_buffer.h
    d3d12_texture.cpp
    d3d12_texture.h
    dinput_source.cpp
    dinput_source.h
    http_downloader_winhttp.cpp
    http_downloader_winhttp.h
    platform_misc_win32.cpp
    win32_raw_input_source.cpp
    win32_raw_input_source.h
    xinput_source.cpp
    xinput_source.h
  )
  target_link_libraries(util PRIVATE d3d12ma)
  target_link_libraries(util PRIVATE d3d11.lib d3d12.lib d3dcompiler.lib dxgi.lib winmm.lib Dwmapi.lib winhttp.lib)

  if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    target_link_libraries(util PRIVATE WinPixEventRuntime::WinPixEventRuntime)
  endif()
elseif(APPLE)
  include(AddMetalSources)

  set(MAC_SOURCES
    metal_device.h
    metal_device.mm
    metal_layer.h
    metal_stream_buffer.h
    metal_stream_buffer.mm
    platform_misc_mac.mm
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
elseif(NOT ANDROID)
  target_sources(util PRIVATE
    platform_misc_unix.cpp
  )
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DBUS REQUIRED dbus-1)
  target_include_directories(util PRIVATE ${DBUS_INCLUDE_DIRS})
  target_link_libraries(util PRIVATE ${DBUS_LINK_LIBRARIES})

  if(LINUX)
    target_link_libraries(util PRIVATE UDEV::UDEV)
  endif()
endif()

if(NOT WIN32 AND NOT ANDROID)
  target_sources(util PRIVATE
    http_downloader_curl.cpp
    http_downloader_curl.h
  )
  target_link_libraries(util PRIVATE
    CURL::libcurl
  )
endif()

function(add_util_resources target)
  if(APPLE)
    get_property(UTIL_METAL_SOURCES GLOBAL PROPERTY UTIL_METAL_SOURCES)
    add_metal_sources(${target} ${UTIL_METAL_SOURCES})

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

    # Copy shaderc/spirv-cross into the bundle
    get_target_property(SPIRV_CROSS_LIBRARY spirv-cross-c-shared IMPORTED_SONAME_RELEASE)
    get_target_property(SHADERC_LIBRARY Shaderc::shaderc_shared IMPORTED_LOCATION_RELEASE)
    target_sources(${target} PRIVATE "${SHADERC_LIBRARY}" "${SPIRV_CROSS_LIBRARY}")
    set_source_files_properties("${SHADERC_LIBRARY}" "${SPIRV_CROSS_LIBRARY}" PROPERTIES MACOSX_PACKAGE_LOCATION Frameworks)

    # Copy FFmpeg libraries into the bundle
    foreach(component avcodec avformat avutil swresample swscale)
      string(REGEX REPLACE "\([0-9]+\)\.[0-9]+\.[0-9]+" "\\1" major "${FFMPEG_${component}_VERSION}")
      string(REPLACE "lib${component}.dylib" "lib${component}.${major}.dylib" version_lib "${FFMPEG_${component}_LIBRARIES}")
      target_sources(${target} PRIVATE ${version_lib})
      set_source_files_properties(${target} PRIVATE ${version_lib} PROPERTIES MACOSX_PACKAGE_LOCATION Frameworks)
    endforeach()
  endif()
endfunction()
