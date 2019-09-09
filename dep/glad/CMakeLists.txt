set(SRCS
    src/glad.c
)

add_library(glad ${SRCS})
target_include_directories(glad PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(glad INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(glad Threads::Threads "${CMAKE_DL_LIBS}")
