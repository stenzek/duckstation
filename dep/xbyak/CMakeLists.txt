add_library(xbyak INTERFACE)
target_include_directories(xbyak INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/xbyak")
target_compile_definitions(xbyak INTERFACE "XBYAK_NO_EXCEPTION=1")
