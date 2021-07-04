set(SRCS
  include/imconfig.h
  include/imgui.h
  include/imgui_internal.h
  include/imgui_stdlib.h
  include/imstb_textedit.h
  src/imgui.cpp
  src/imgui_demo.cpp
  src/imgui_draw.cpp
  src/imgui_stdlib.cpp
  src/imgui_tables.cpp
  src/imgui_widgets.cpp
  src/imstb_rectpack.h
  src/imstb_truetype.h
)

add_library(imgui ${SRCS})
target_include_directories(imgui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(imgui INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

