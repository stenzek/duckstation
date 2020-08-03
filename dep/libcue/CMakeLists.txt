set(SRCS
    include/libcue/cd.h
    include/libcue/cdtext.h
    include/libcue/libcue.h
    include/libcue/rem.h
    include/libcue/cuetime.h
    src/cd.c
    src/cdtext.c
    src/cue_parser.c
    src/cue_parser.h
    src/cue_scanner.c
    src/rem.c
    src/time.c
)

add_library(libcue ${SRCS})
target_include_directories(libcue PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/libcue" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(libcue INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(libcue PROPERTIES LINKER_LANGUAGE C)

