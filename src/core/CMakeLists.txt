add_library(core
    bus.cpp
    bus.h
    bus.inl
    cdrom.cpp
    cdrom.h
    cpu_core.cpp
    cpu_core.h
    cpu_core.inl
    cpu_disasm.cpp
    cpu_disasm.h
    cpu_types.h
    digital_controller.cpp
    digital_controller.h
    dma.cpp
    dma.h
    gpu.cpp
    gpu.h
    gpu_commands.cpp
    gpu_hw.cpp
    gpu_hw.h
    gpu_hw_opengl.cpp
    gpu_hw_opengl.h
    gte.cpp
    gte.h
    gte.inl
    gte_types.h
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
    pad_device.cpp
    pad_device.h
    save_state_version.h
    settings.cpp
    settings.h
    spu.cpp
    spu.h
    system.cpp
    system.h
    timers.cpp
    timers.h
    types.h
)

target_include_directories(core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(core Threads::Threads YBaseLib common imgui stb)

