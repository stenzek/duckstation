add_library(core
    achievements.h
    analog_controller.cpp
    analog_controller.h
    analog_joystick.cpp
    analog_joystick.h
    bios.cpp
    bios.h
    bus.cpp
    bus.h
    cdrom.cpp
    cdrom.h
    cdrom_async_reader.cpp
    cdrom_async_reader.h
    cheats.cpp
    cheats.h
    controller.cpp
    controller.h
    common_host.cpp
    common_host.h
    cpu_code_cache.cpp
    cpu_code_cache.h
    cpu_core.cpp
    cpu_core.h
    cpu_core_private.h
    cpu_disasm.cpp
    cpu_disasm.h
    cpu_types.cpp
    cpu_types.h
    digital_controller.cpp
    digital_controller.h
    dma.cpp
    dma.h
    fullscreen_ui.cpp
    fullscreen_ui.h
    game_database.cpp
    game_database.h
    game_list.cpp
    game_list.h
    gdb_protocol.cpp
    gdb_protocol.h
    gpu.cpp
    gpu.h
    gpu_backend.cpp
    gpu_backend.h
    gpu_commands.cpp
    gpu_hw.cpp
    gpu_hw.h
    gpu_hw_shadergen.cpp
    gpu_hw_shadergen.h
    gpu_sw.cpp
    gpu_sw.h
    gpu_sw_backend.cpp
    gpu_sw_backend.h
    gpu_types.h
    guncon.cpp
    guncon.h
    gte.cpp
    gte.h
    gte_types.h
    host.cpp
    host.h
    host_interface_progress_callback.cpp
    host_interface_progress_callback.h
    host_settings.cpp
    host_settings.h
    input_types.h
    imgui_overlays.cpp
    imgui_overlays.h
    interrupt_controller.cpp
    interrupt_controller.h
    mdec.cpp
    mdec.h
    memory_card.cpp
    memory_card.h
    memory_card_image.cpp
    memory_card_image.h
    multitap.cpp
    multitap.h
    negcon.cpp
    negcon.h
    pad.cpp
    pad.h
    pcdrv.cpp
    pcdrv.h
    pgxp.cpp
    pgxp.h
    playstation_mouse.cpp
    playstation_mouse.h
    psf_loader.cpp
    psf_loader.h
    resources.cpp
    resources.h
    save_state_version.h
    settings.cpp
    settings.h
    shader_cache_version.h
    sio.cpp
    sio.h
    spu.cpp
    spu.h
    system.cpp
    system.h
    texture_replacements.cpp
    texture_replacements.h
    timers.cpp
    timers.h
    timing_event.cpp
    timing_event.h
    types.h
)

set(RECOMPILER_SRCS
    cpu_recompiler_code_generator.cpp
    cpu_recompiler_code_generator.h
    cpu_recompiler_code_generator_generic.cpp
    cpu_recompiler_register_cache.cpp
    cpu_recompiler_register_cache.h
    cpu_recompiler_thunks.h
    cpu_recompiler_types.h
)

target_include_directories(core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(core PUBLIC Threads::Threads common util zlib)
target_link_libraries(core PRIVATE stb xxhash imgui rapidjson)

if(${CPU_ARCH} STREQUAL "x64")
  target_include_directories(core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../dep/xbyak/xbyak")
  target_compile_definitions(core PUBLIC "WITH_RECOMPILER=1" "WITH_MMAP_FASTMEM=1")
  target_sources(core PRIVATE ${RECOMPILER_SRCS}
    cpu_recompiler_code_generator_x64.cpp
  )
  message("Building x64 recompiler")
elseif(${CPU_ARCH} STREQUAL "aarch32")
  target_compile_definitions(core PUBLIC "WITH_RECOMPILER=1")
  target_sources(core PRIVATE ${RECOMPILER_SRCS}
    cpu_recompiler_code_generator_aarch32.cpp
  )
  target_link_libraries(core PUBLIC vixl)
  message("Building AArch32 recompiler")
elseif(${CPU_ARCH} STREQUAL "aarch64")
  target_compile_definitions(core PUBLIC "WITH_RECOMPILER=1")
  if (NOT APPLE)
    # Disabled until we support 16K pages.
    target_compile_definitions(core PUBLIC "WITH_MMAP_FASTMEM=1")
  endif()
  target_sources(core PRIVATE ${RECOMPILER_SRCS}
    cpu_recompiler_code_generator_aarch64.cpp
  )
  target_link_libraries(core PUBLIC vixl)
  message("Building AArch64 recompiler")
else()
  message("Not building recompiler")
endif()

if(ENABLE_CHEEVOS)
  target_sources(core PRIVATE
    achievements.cpp
    achievements_private.h
  )
  target_compile_definitions(core PUBLIC -DWITH_CHEEVOS=1)
  target_link_libraries(core PRIVATE rcheevos rapidjson)
endif()

if(ENABLE_DISCORD_PRESENCE)
  target_compile_definitions(core PUBLIC -DWITH_DISCORD_PRESENCE=1)
  target_link_libraries(core PRIVATE discord-rpc)
endif()

# Copy the provided data directory to the output directory.
add_custom_command(TARGET core POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)
