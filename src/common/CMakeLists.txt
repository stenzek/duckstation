set(SRCS
    audio.cpp
    audio.h
    bitfield.h
    display.cpp
    display.h
    display_renderer.cpp
    display_renderer.h
    display_timing.cpp
    display_timing.h
    fastjmp.h
    hdd_image.cpp
    hdd_image.h
    jit_code_buffer.cpp
    jit_code_buffer.h
    object.cpp
    object.h
    object_type_info.cpp
    object_type_info.h
    property.cpp
    property.h
    state_wrapper.cpp
    state_wrapper.h
    types.h
    type_registry.h
)

add_library(common ${SRCS})

target_include_directories(common PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(common YBaseLib glad libsamplerate Threads::Threads)

if(ENABLE_OPENGL)
  target_sources(common PRIVATE display_renderer_gl.cpp display_renderer_gl.h)
  target_link_libraries(common glad)
endif()

if(MSVC)
  target_sources(common PRIVATE display_renderer_d3d.cpp display_renderer_d3d.h)
  target_link_libraries(common d3d11.lib)
endif()

if(MSVC)
  enable_language(ASM_MASM)
  if(CMAKE_ASM_MASM_COMPILER_WORKS)
    target_sources(common PRIVATE fastjmp.asm)
  else()
    message(ERROR "MASM assembler does not work")
  endif()
endif()

