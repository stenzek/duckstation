set(SRCS
  include/nfd.h
  src/common.h
  src/nfd_common.c
  src/nfd_common.h
)

add_library(nativefiledialog ${SRCS})
target_include_directories(nativefiledialog PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(nativefiledialog INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(WIN32)
  target_sources(nativefiledialog PRIVATE src/nfd_win.cpp)
elseif(APPLE)
  target_sources(nativefiledialog PRIVATE src/nfd_coca.m)
else()
  find_package(GTK2 2.6 COMPONENTS gtk)
  if(GTK2_FOUND)
    message("Using GTK for nativefiledialog")
    target_sources(nativefiledialog PRIVATE src/nfd_gtk.c)
    target_include_directories(nativefiledialog PRIVATE ${GTK2_INCLUDE_DIRS})
    target_link_libraries(nativefiledialog PRIVATE ${GTK2_LIBRARIES})
  else()
    message(WARNING "Unknown platform for nativefiledialog")
  endif()
endif()

