set(SRCS
    src/glad.c
)

# Linking as a static library breaks on macOS, see https://github.com/libigl/libigl/issues/751
if(APPLE)
  add_library(glad OBJECT ${SRCS})
else()
  add_library(glad ${SRCS})
endif()

target_include_directories(glad PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(glad INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(glad PRIVATE Threads::Threads "${CMAKE_DL_LIBS}")

if(WIN32)
  target_sources(glad PRIVATE src/glad_wgl.c)
else()
  if(USE_EGL)
    target_sources(glad PRIVATE src/glad_egl.c)
    target_link_libraries(glad PRIVATE EGL::EGL)
  endif()
  if(USE_X11)
    target_sources(glad PRIVATE src/glad_glx.c)
  endif()
endif()
