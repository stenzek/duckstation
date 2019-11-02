set(SRCS
    audio_stream.cpp
    audio_stream.h
    bitfield.h
    cd_image.cpp
    cd_image.h
    cd_image_bin.cpp
    cd_image_cue.cpp
    cd_xa.cpp
    cd_xa.h
    gl_program.cpp
    gl_program.h
    gl_stream_buffer.cpp
    gl_stream_buffer.h
    gl_texture.cpp
    gl_texture.h
    jit_code_buffer.cpp
    jit_code_buffer.h
    state_wrapper.cpp
    state_wrapper.h
    types.h
)

add_library(common ${SRCS})

target_include_directories(common PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(common YBaseLib glad libcue Threads::Threads)
