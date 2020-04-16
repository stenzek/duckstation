if(WIN32)
  add_custom_command(OUTPUT scmversion.cpp COMMAND "cmd /k gen_scmversion.bat" WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
else()
  add_custom_command(OUTPUT scmversion.cpp COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/gen_scmversion.sh")
endif()

add_library(scmversion
  scmversion.cpp
  scmversion.h
)

