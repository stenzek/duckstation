set(SRCS
    include/samplerate.h
    src/common.h
    src/config.h
    src/fastest_coeffs.h
    src/float_cast.h
    src/high_qual_coeffs.h
    src/mid_qual_coeffs.h
    src/samplerate.c
    src/src_linear.c
    src/src_sinc.c
    src/src_zoh.c
)

add_library(libsamplerate ${SRCS})
target_include_directories(libsamplerate PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(libsamplerate INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(libsamplerate PROPERTIES LINKER_LANGUAGE C)

