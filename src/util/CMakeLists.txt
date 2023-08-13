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
  host_display.cpp
  host_display.h
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
  jit_code_buffer.cpp
  jit_code_buffer.h
  memory_arena.cpp
  memory_arena.h
  page_fault_handler.cpp
  page_fault_handler.h
  platform_misc.h
  postprocessing_chain.cpp
  postprocessing_chain.h
  postprocessing_shader.cpp
  postprocessing_shader.h
  postprocessing_shadergen.cpp
  postprocessing_shadergen.h
  shadergen.cpp
  shadergen.h
  shiftjis.cpp
  shiftjis.h
  state_wrapper.cpp
  state_wrapper.h
  wav_writer.cpp
  wav_writer.h
)

target_include_directories(util PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(util PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(util PUBLIC common simpleini imgui)
target_link_libraries(util PRIVATE stb libchdr zlib soundtouch)

if(ENABLE_CUBEB)
  target_sources(util PRIVATE
    cubeb_audio_stream.cpp
    cubeb_audio_stream.h
  )
  target_compile_definitions(util PUBLIC "WITH_CUBEB=1")
  target_link_libraries(util PRIVATE cubeb)
endif()

if(ENABLE_OPENGL)
  target_sources(util PRIVATE
    opengl_host_display.cpp
    opengl_host_display.h
    imgui_impl_opengl3.cpp
    imgui_impl_opengl3.h
  )
  target_link_libraries(util PRIVATE glad)
endif()

if(ENABLE_VULKAN)
  target_sources(util PRIVATE
    imgui_impl_vulkan.cpp
    imgui_impl_vulkan.h
    vulkan_host_display.cpp
    vulkan_host_display.h
  )
endif()

if(SDL2_FOUND)
  target_sources(util PRIVATE 
    sdl_input_source.cpp
    sdl_input_source.h
  )
  target_compile_definitions(util PUBLIC "WITH_SDL2=1")
  target_include_directories(util PUBLIC ${SDL2_INCLUDE_DIRS})
  target_link_libraries(util PUBLIC ${SDL2_LIBRARIES})

  # Copy bundled SDL2 to output on Windows.
  if(WIN32)
    add_custom_command(TARGET util POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SDL2_DLL_PATH}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/SDL2.dll")
  endif()
endif()

if(USE_X11)
  target_compile_definitions(util PRIVATE "-DUSE_X11=1")
  target_include_directories(util PRIVATE "${X11_INCLUDE_DIR}")
endif()

if(USE_DBUS)
	target_compile_definitions(util PRIVATE USE_DBUS)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(DBUS REQUIRED dbus-1)
	target_include_directories(util PRIVATE ${DBUS_INCLUDE_DIRS})
	target_link_libraries(util PRIVATE ${DBUS_LINK_LIBRARIES})
endif()

if(WIN32)
  target_sources(util PRIVATE
    d3d11_host_display.cpp
    d3d11_host_display.h
    d3d12_host_display.cpp
    d3d12_host_display.h
    dinput_source.cpp
    dinput_source.h
    imgui_impl_dx11.cpp
    imgui_impl_dx11.h
    imgui_impl_dx12.cpp
    imgui_impl_dx12.h
    platform_misc_win32.cpp
    win32_raw_input_source.cpp
    win32_raw_input_source.h
    xaudio2_audio_stream.cpp
    xaudio2_audio_stream.h
    xinput_source.cpp
    xinput_source.h
  )
  target_link_libraries(util PRIVATE d3d11.lib dxgi.lib winmm.lib)
elseif(APPLE)
  find_library(IOK_LIBRARY IOKit REQUIRED)
  target_link_libraries(util PRIVATE "${IOK_LIBRARY}")
  target_sources(util PRIVATE
    platform_misc_mac.mm
  )
elseif(NOT ANDROID)
  target_sources(util PRIVATE
    platform_misc_unix.cpp
  )
endif()
