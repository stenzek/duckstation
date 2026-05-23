# SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0 + Packaging Restriction
#
# NOTE: In addition to the terms of CC-BY-NC-ND-4.0, you may not use this file to create
# packages or build recipes without explicit permission from the copyright holder.

add_library(common
  align.h
  assert.cpp
  assert.h
  binary_reader_writer.cpp
  binary_reader_writer.h
  bitfield.h
  bitutils.h
  bcdutils.h
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
  locked_ptr.h
  log.cpp
  log.h
  log_channels.h
  memmap.cpp
  memmap.h
  md5_digest.cpp
  md5_digest.h
  minizip_helpers.h
  optional_with_status.h
  path.h
  perf_scope.cpp
  perf_scope.h
  progress_callback.cpp
  progress_callback.h
  ryml_helpers.h
  scoped_guard.h
  settings_interface.cpp
  settings_interface.h
  sha1_digest.cpp
  sha1_digest.h
  sha256_digest.cpp
  sha256_digest.h
  small_string.cpp
  small_string.h
  string_util.cpp
  string_util.h
  string_pool.cpp
  string_pool.h
  time_helpers.h
  thirdparty/SmallVector.cpp
  thirdparty/SmallVector.h
  thirdparty/aes.cpp
  thirdparty/aes.h
  thirdparty/usb_key_code_data.h
  thirdparty/usb_key_code_data.inl
  task_queue.cpp
  task_queue.h
  threading.cpp
  threading.h
  timer.cpp
  timer.h
  types.h
  xorshift_prng.h
)

target_include_directories(common PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(common PUBLIC fmt Threads::Threads)
target_link_libraries(common PRIVATE fast_float "${CMAKE_DL_LIBS}")

if(APPLE)
  set(MAC_SOURCES
    cocoa_tools.h
    cocoa_tools.mm
  )
  target_sources(common PRIVATE ${MAC_SOURCES})
  set_source_files_properties(${MAC_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)
  find_library(COCOA_LIBRARY Cocoa REQUIRED)
  find_library(QUARTZCORE_LIBRARY QuartzCore)
  target_link_libraries(common PRIVATE ${COCOA_LIBRARY} ${QUARTZCORE_LIBRARY})
endif()

if(NOT ANDROID AND NOT APPLE)
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
if(DEFINED HOST_MIN_PAGE_SIZE AND DEFINED HOST_MAX_PAGE_SIZE)
  target_compile_definitions(common PUBLIC
    "-DMIN_HOST_PAGE_SIZE=${HOST_MIN_PAGE_SIZE}"
    "-DMAX_HOST_PAGE_SIZE=${HOST_MAX_PAGE_SIZE}"
  )
elseif(DEFINED HOST_PAGE_SIZE)
  target_compile_definitions(common PUBLIC
    "-DOVERRIDE_HOST_PAGE_SIZE=${HOST_PAGE_SIZE}"
  )
endif()
if(DEFINED HOST_CACHE_LINE_SIZE)
	target_compile_definitions(common PUBLIC
    "-DOVERRIDE_HOST_CACHE_LINE_SIZE=${HOST_CACHE_LINE_SIZE}"
  )
endif()
