set(SRCS
  include/imconfig.h
  include/imgui.h
  include/imgui_stdlib.h
  src/imgui.cpp
  src/imgui_demo.cpp
  src/imgui_draw.cpp
  src/imgui_widgets.cpp
  src/imgui_internal.h
  src/imstb_rectpack.h
  src/imgui_stdlib.cpp
  src/imstb_textedit.h
  src/imstb_truetype.h
)

add_library(imgui ${SRCS})
target_include_directories(imgui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(imgui INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(imgui PRIVATE "imgui_STATIC")

target_sources(imgui PRIVATE
  include/imgui_impl_opengl3.h
  src/imgui_impl_opengl3.cpp
)
target_link_libraries(imgui PRIVATE glad)

if(WIN32)
  target_sources(imgui PRIVATE include/imgui_impl_dx11.h src/imgui_impl_dx11.cpp)
endif()


