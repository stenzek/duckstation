if(NOT DISABLE_GEN_SCM_VERSION)
  if(WIN32)
    # _scmversion.cpp is a dummy file that will never be created, so the command will always be run
    add_custom_command(OUTPUT scmversion.cpp _scmversion.cpp
                      COMMAND cmd /k "${CMAKE_CURRENT_SOURCE_DIR}/gen_scmversion.bat"
                      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  else()
    add_custom_command(OUTPUT scmversion.cpp PRE_BUILD
                      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/gen_scmversion.sh")
  endif()
endif()

add_library(scmversion
  scmversion.cpp
  scmversion.h
)

