add_library(rcheevos
  include/rc_consoles.h
  include/rc_hash.h
  include/rc_url.h
  include/rcheevos.h
  src/rcheevos/alloc.c
  src/rcheevos/compat.c
  src/rcheevos/condition.c
  src/rcheevos/condset.c
  src/rcheevos/consoleinfo.c
  src/rcheevos/format.c
  src/rcheevos/lboard.c
  src/rcheevos/memref.c
  src/rcheevos/operand.c
  src/rcheevos/rc_compat.h
  src/rcheevos/rc_internal.h
  src/rcheevos/richpresence.c
  src/rcheevos/runtime.c
  src/rcheevos/runtime_progress.c
  src/rcheevos/trigger.c
  src/rcheevos/value.c
  src/rhash/cdreader.c
  src/rhash/hash.c
  src/rhash/md5.c
  src/rhash/md5.h
  src/rurl/url.c
)

target_include_directories(rcheevos PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(rcheevos INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(rcheevos PRIVATE "RC_DISABLE_LUA=1")

