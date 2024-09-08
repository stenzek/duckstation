add_library(common
  align.h
  assert.cpp
  assert.h
  binary_reader_writer.cpp
  binary_reader_writer.h
  bitfield.h
  bitutils.h
  crash_handler.cpp
  crash_handler.h
  dimensional_array.h
  dynamic_library.cpp
  dynamic_library.h
  error.cpp
  error.h
  fastjmp.cpp
  fastjmp.h
  fifo_queue.h
  file_system.cpp
  file_system.h
  gsvector.cpp
  gsvector.h
  gsvector_formatter.h
  gsvector_neon.h
  gsvector_nosimd.h
  gsvector_sse.h
  intrin.h
  hash_combine.h
  heap_array.h
  heterogeneous_containers.h
  layered_settings_interface.cpp
  layered_settings_interface.h
  log.cpp
  log.h
  memmap.cpp
  memmap.h
  md5_digest.cpp
  md5_digest.h
  memory_settings_interface.cpp
  memory_settings_interface.h
  minizip_helpers.h
  path.h
  perf_scope.cpp
  perf_scope.h
  progress_callback.cpp
  progress_callback.h
  ryml_helpers.h
  scoped_guard.h
  settings_interface.h
  sha1_digest.cpp
  sha1_digest.h
  small_string.cpp
  small_string.h
  string_util.cpp
  string_util.h
  thirdparty/SmallVector.cpp
  thirdparty/SmallVector.h
  threading.cpp
  threading.h
  timer.cpp
  timer.h
  types.h
)

target_include_directories(common PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(common PUBLIC fmt Threads::Threads fast_float)
target_link_libraries(common PRIVATE "${CMAKE_DL_LIBS}")

if(WIN32)
  target_sources(common PRIVATE
    thirdparty/StackWalker.cpp
    thirdparty/StackWalker.h
    windows_headers.h
  )
  target_link_libraries(common PRIVATE OneCore.lib)
endif()

if(MSVC)
  if(CPU_ARCH_X64)
    enable_language(ASM_MASM)
    target_sources(common PRIVATE fastjmp_x86.asm)
    set_source_files_properties(fastjmp_x86.asm PROPERTIES COMPILE_FLAGS "/D_M_X86_64")
  elseif(CPU_ARCH_ARM32 OR CPU_ARCH_ARM64)
    enable_language(ASM_MARMASM)
    target_sources(common PRIVATE fastjmp_arm.asm)
  endif()
endif()

if(APPLE)
  set(MAC_SOURCES
    cocoa_tools.h
    cocoa_tools.mm
  )
  target_sources(common PRIVATE ${MAC_SOURCES})
  set_source_files_properties(${MAC_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)
  find_library(COCOA_LIBRARY Cocoa REQUIRED)
  target_link_libraries(common PRIVATE ${COCOA_LIBRARY})
endif()

if(NOT WIN32 AND NOT ANDROID AND NOT APPLE)
  target_link_libraries(common PRIVATE libbacktrace::libbacktrace)
endif()

if(ANDROID)
  target_link_libraries(common PRIVATE log)
endif()

if(LINUX)
  # We need -lrt for shm_unlink
  target_link_libraries(common PRIVATE rt)
endif()

# If the host size was detected, we need to set it as a macro.
if(HOST_PAGE_SIZE)
  target_compile_definitions(common PUBLIC "-DOVERRIDE_HOST_PAGE_SIZE=${HOST_PAGE_SIZE}")
endif()
if(HOST_CACHE_LINE_SIZE)
	target_compile_definitions(common PUBLIC "-DOVERRIDE_HOST_CACHE_LINE_SIZE=${HOST_CACHE_LINE_SIZE}")
endif()
