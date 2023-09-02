add_library(d3d12ma
	include/D3D12MemAlloc.h
	src/D3D12MemAlloc.cpp
)

target_include_directories(d3d12ma PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
