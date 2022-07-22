add_library(frontend-common
  achievements.cpp
  achievements.h
  common_host.cpp
  common_host.h
  cubeb_audio_stream.cpp
  cubeb_audio_stream.h
  fullscreen_ui.cpp
  fullscreen_ui.h
  game_list.cpp
  game_list.h
  host_settings.cpp
  icon.cpp
  icon.h
  inhibit_screensaver.cpp
  inhibit_screensaver.h
  input_manager.cpp
  input_manager.h
  input_source.cpp
  input_source.h
  imgui_fullscreen.cpp
  imgui_fullscreen.h
  imgui_impl_opengl3.cpp
  imgui_impl_opengl3.h
  imgui_impl_vulkan.cpp
  imgui_impl_vulkan.h
  imgui_manager.cpp
  imgui_manager.h
  imgui_overlays.cpp
  imgui_overlays.h
  opengl_host_display.cpp
  opengl_host_display.h
  postprocessing_chain.cpp
  postprocessing_chain.h
  postprocessing_shader.cpp
  postprocessing_shader.h
  postprocessing_shadergen.cpp
  postprocessing_shadergen.h
  vulkan_host_display.cpp
  vulkan_host_display.h
)

target_link_libraries(frontend-common PUBLIC core common glad cubeb imgui tinyxml2 rapidjson scmversion)

if(WIN32)
  target_sources(frontend-common PRIVATE
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
    win32_raw_input_source.cpp
    win32_raw_input_source.h
    xaudio2_audio_stream.cpp
    xaudio2_audio_stream.h
    xinput_source.cpp
    xinput_source.h
  )
  target_link_libraries(frontend-common PRIVATE d3d11.lib dxgi.lib)
endif()

if(APPLE)
  find_library(IOK_LIBRARY IOKit REQUIRED)
  target_link_libraries(frontend-common PRIVATE "${IOK_LIBRARY}")
endif()

if(SDL2_FOUND)
  target_sources(frontend-common PRIVATE 
    sdl_audio_stream.cpp
    sdl_audio_stream.h
    sdl_input_source.cpp
    sdl_input_source.h
    sdl_initializer.cpp
    sdl_initializer.h
  )
  target_compile_definitions(frontend-common PUBLIC "WITH_SDL2=1")
  target_include_directories(frontend-common PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(frontend-common PRIVATE ${SDL2_LIBRARIES})

  # Copy bundled SDL2 to output on Windows.
  if(WIN32)
    add_custom_command(TARGET frontend-common POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SDL2_DLL_PATH}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/SDL2.dll")
  endif()
endif()

if(USE_EVDEV)
  target_compile_definitions(frontend-common PUBLIC "-DWITH_EVDEV=1")
  target_include_directories(frontend-common PRIVATE ${LIBEVDEV_INCLUDE_DIRS})
  target_link_libraries(frontend-common PRIVATE ${LIBEVDEV_LIBRARIES})
  target_sources(frontend-common PRIVATE
    evdev_controller_interface.cpp
    evdev_controller_interface.h
  )
endif()

if(USE_X11)
  target_compile_definitions(frontend-common PRIVATE "-DUSE_X11=1")
  target_include_directories(frontend-common PRIVATE "${X11_INCLUDE_DIR}")
endif()

if(ENABLE_DISCORD_PRESENCE)
  target_compile_definitions(frontend-common PUBLIC -DWITH_DISCORD_PRESENCE=1)
  target_link_libraries(frontend-common PRIVATE discord-rpc)
endif()

if(ENABLE_CHEEVOS)
  target_sources(frontend-common PRIVATE
    achievements.cpp
    achievements.h
  )
  target_compile_definitions(frontend-common PUBLIC -DWITH_CHEEVOS=1)
  target_link_libraries(frontend-common PRIVATE rcheevos rapidjson)
endif()

# Copy the provided data directory to the output directory.
add_custom_command(TARGET frontend-common POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)
