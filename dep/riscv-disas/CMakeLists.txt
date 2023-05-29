add_library(riscv-disas
	include/riscv-disas.h
	src/riscv-disas.c
)

target_include_directories(riscv-disas PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

