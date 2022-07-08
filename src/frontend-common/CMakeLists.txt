add_library(frontend-common
  common_host_interface.cpp
  common_host_interface.h
  controller_interface.cpp
  controller_interface.h
  cubeb_audio_stream.cpp
  cubeb_audio_stream.h
  fullscreen_ui.cpp
  fullscreen_ui.h
  fullscreen_ui_progress_callback.cpp
  fullscreen_ui_progress_callback.h
  game_database.cpp
  game_database.h
  game_list.cpp
  game_list.h
  game_settings.cpp
  game_settings.h
  host_settings.cpp
  icon.cpp
  icon.h
  inhibit_screensaver.cpp
  inhibit_screensaver.h
  ini_settings_interface.cpp
  ini_settings_interface.h
  input_overlay_ui.cpp
  input_overlay_ui.h
  imgui_impl_opengl3.cpp
  imgui_impl_opengl3.h
  imgui_impl_vulkan.cpp
  imgui_impl_vulkan.h
  opengl_host_display.cpp
  opengl_host_display.h
  postprocessing_chain.cpp
  postprocessing_chain.h
  postprocessing_shader.cpp
  postprocessing_shader.h
  postprocessing_shadergen.cpp
  postprocessing_shadergen.h
  save_state_selector_ui.cpp
  save_state_selector_ui.h
  vulkan_host_display.cpp
  vulkan_host_display.h
)

target_link_libraries(frontend-common PUBLIC core common glad cubeb imgui simpleini tinyxml2 rapidjson scmversion)

if(WIN32)
  target_sources(frontend-common PRIVATE
    d3d11_host_display.cpp
    d3d11_host_display.h
    d3d12_host_display.cpp
    d3d12_host_display.h
    dinput_controller_interface.cpp
    dinput_controller_interface.h
    imgui_impl_dx11.cpp
    imgui_impl_dx11.h
    imgui_impl_dx12.cpp
    imgui_impl_dx12.h
    xaudio2_audio_stream.cpp
    xaudio2_audio_stream.h
    xinput_controller_interface.cpp
    xinput_controller_interface.h
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
    sdl_controller_interface.cpp
    sdl_controller_interface.h
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

# Copy the provided data directory to the output directory.
add_custom_command(TARGET frontend-common POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)
