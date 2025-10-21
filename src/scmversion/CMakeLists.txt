if(CMAKE_HOST_SYSTEM MATCHES "Windows")
  add_custom_target(generate_scmversion
                    COMMAND cmd /k "${CMAKE_CURRENT_SOURCE_DIR}/gen_scmversion.bat"
                    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
                    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/scmversion.cpp"
                    COMMENT "Checking if scmversion.cpp needs to be updated")
else()
  add_custom_target(generate_scmversion
                    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/gen_scmversion.sh"
                    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
                    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/scmversion.cpp"
                    COMMENT "Checking if scmversion.cpp needs to be updated")
endif()

add_library(scmversion
  "${CMAKE_CURRENT_BINARY_DIR}/scmversion.cpp"
  scmversion.h
)
add_dependencies(scmversion generate_scmversion)
