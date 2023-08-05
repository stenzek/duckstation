set(SRCS
	include/spirv-cross/GLSL.std.450.h
	include/spirv-cross/spirv.h
	include/spirv-cross/spirv.hpp
	include/spirv-cross/spirv_cfg.hpp
	include/spirv-cross/spirv_common.hpp
	include/spirv-cross/spirv_cpp.hpp
	include/spirv-cross/spirv_cross.hpp
	include/spirv-cross/spirv_cross_containers.hpp
	include/spirv-cross/spirv_cross_error_handling.hpp
	include/spirv-cross/spirv_cross_parsed_ir.hpp
	include/spirv-cross/spirv_cross_util.hpp
	include/spirv-cross/spirv_glsl.hpp
	include/spirv-cross/spirv_hlsl.hpp
	include/spirv-cross/spirv_msl.hpp
	include/spirv-cross/spirv_parser.hpp
	include/spirv-cross/spirv_reflect.hpp
	src/spirv_cfg.cpp
	src/spirv_cpp.cpp
	src/spirv_cross.cpp
	src/spirv_cross_parsed_ir.cpp
	src/spirv_cross_util.cpp
	src/spirv_glsl.cpp
	src/spirv_hlsl.cpp
	src/spirv_msl.cpp
	src/spirv_parser.cpp
	src/spirv_reflect.cpp
)

add_library(spirv-cross ${SRCS})

target_include_directories(spirv-cross PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/spirv-cross")
target_include_directories(spirv-cross PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(spirv-cross PUBLIC SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS)

