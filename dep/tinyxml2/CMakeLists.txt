set(SRCS
  include/tinyxml2.h
  src/tinyxml2.cpp
)

if(WIN32)
  add_library(tinyxml2 SHARED ${SRCS})
  target_include_directories(tinyxml2 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_definitions(tinyxml2 PRIVATE "TINYXML2_EXPORT")
  target_compile_definitions(tinyxml2 INTERFACE "TINYXML2_IMPORT")
else()
  add_library(tinyxml2 ${SRCS})
  target_include_directories(tinyxml2 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
endif()
