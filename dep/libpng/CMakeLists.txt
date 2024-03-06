add_library(png
  src/png.c
  src/pngerror.c
  src/pngget.c
  src/pngmem.c
  src/pngpread.c
  src/pngread.c
  src/pngrio.c
  src/pngrtran.c
  src/pngrutil.c
  src/pngset.c
  src/pngtrans.c
  src/pngwio.c
  src/pngwrite.c
  src/pngwtran.c
  src/pngwutil.c
)

if(CPU_ARCH_X64)
  target_sources(png PRIVATE
    src/intel/filter_sse2_intrinsics.c
    src/intel/intel_init.c
  )
  target_compile_definitions(png PRIVATE "PNG_INTEL_SSE")
elseif(CPU_ARCH_ARM32 OR CPU_ARCH_ARM64)
  target_sources(png PRIVATE
    src/arm/arm_init.c
    src/arm/filter_neon.S
    src/arm/filter_neon_intrinsics.c
    src/arm/palette_neon_intrinsics.c
  )
endif()

target_link_libraries(png PRIVATE ZLIB::ZLIB)
target_include_directories(png PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(png PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
disable_compiler_warnings_for_target(png)

add_library(PNG::PNG ALIAS png)
