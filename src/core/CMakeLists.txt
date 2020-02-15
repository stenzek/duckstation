add_library(core
    analog_controller.cpp
    analog_controller.h
    bios.cpp
    bios.h
    bus.cpp
    bus.h
    bus.inl
    cdrom.cpp
    cdrom.h
    controller.cpp
    controller.h
    cpu_code_cache.cpp
    cpu_code_cache.h
    cpu_core.cpp
    cpu_core.h
    cpu_core.inl
    cpu_disasm.cpp
    cpu_disasm.h
    cpu_types.cpp
    cpu_types.h
    digital_controller.cpp
    digital_controller.h
    dma.cpp
    dma.h
    game_list.cpp
    game_list.h
    gpu.cpp
    gpu.h
    gpu_commands.cpp
    gpu_hw.cpp
    gpu_hw.h
    gpu_hw_opengl.cpp
    gpu_hw_opengl.h
    gpu_hw_opengl_es.cpp
    gpu_hw_opengl_es.h
    gpu_hw_shadergen.cpp
    gpu_hw_shadergen.h
    gpu_sw.cpp
    gpu_sw.h
    gte.cpp
    gte.h
    gte.inl
    gte_types.h
    host_display.cpp
    host_display.h
    host_interface.cpp
    host_interface.h
    interrupt_controller.cpp
    interrupt_controller.h
    mdec.cpp
    mdec.h
    memory_card.cpp
    memory_card.h
    pad.cpp
    pad.h
    save_state_version.h
    settings.cpp
    settings.h
    sio.cpp
    sio.h
    spu.cpp
    spu.h
    system.cpp
    system.h
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
    cpu_recompiler_thunks.cpp
    cpu_recompiler_thunks.h
    cpu_recompiler_types.h
)

target_include_directories(core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(core PUBLIC Threads::Threads common imgui tinyxml2)
target_link_libraries(core PRIVATE glad stb)

if(WIN32)
  target_sources(core PRIVATE
    gpu_hw_d3d11.cpp
    gpu_hw_d3d11.h
  )
  target_link_libraries(core PRIVATE winmm.lib)
endif()

if(${CPU_ARCH} STREQUAL "x64")
  target_include_directories(core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../dep/xbyak/xbyak")
  target_compile_definitions(core PRIVATE "WITH_RECOMPILER=1")
  target_sources(core PRIVATE ${RECOMPILER_SRCS}
    cpu_recompiler_code_generator_x64.cpp
  )
  message("Building x64 recompiler")
elseif(${CPU_ARCH} STREQUAL "aarch64")
  target_compile_definitions(core PRIVATE "WITH_RECOMPILER=1")
  target_sources(core PRIVATE ${RECOMPILER_SRCS}
    cpu_recompiler_code_generator_aarch64.cpp
  )
  target_link_libraries(core PRIVATE vixl)
  message("Building AArch64 recompiler")
else()
  message("Not building recompiler")
endif()
