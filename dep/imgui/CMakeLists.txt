set(SRCS
  include/imconfig.h
  include/imgui.h
  src/imgui.cpp
  src/imgui_demo.cpp
  src/imgui_draw.cpp
  src/imgui_widgets.cpp
  src/imgui_internal.h
  src/imstb_rectpack.h
  src/imstb_textedit.h
  src/imstb_truetype.h
)

add_library(imgui ${SRCS})
target_include_directories(imgui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(imgui INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(imgui PRIVATE "imgui_STATIC" "IMGUI_IMPL_OPENGL_LOADER_GLAD")

if(WIN32)
  target_sources(imgui PRIVATE include/imgui_impl_dx11.h src/imgui_impl_dx11.cpp)
endif()

if(ENABLE_SDL_FRONTEND)
  target_sources(imgui PRIVATE
    include/imgui_impl_opengl3.h
    include/imgui_impl_sdl.h
    src/imgui_impl_opengl3.cpp
    src/imgui_impl_sdl.cpp
  )
  target_include_directories(imgui PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(imgui PRIVATE glad ${SDL2_LIBRARIES})
endif()

