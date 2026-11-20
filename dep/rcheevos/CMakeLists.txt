add_library(rcheevos
  include/rcheevos.h
  include/rc_api_editor.h
  include/rc_api_info.h
  include/rc_api_request.h
  include/rc_api_runtime.h
  include/rc_api_user.h
  include/rc_client.h
  include/rc_client_callbacks.h
  include/rc_consoles.h
  include/rc_error.h
  include/rc_export.h
  include/rc_runtime.h
  include/rc_util.h
  src/api/rc_api_common.c
  src/api/rc_api_common.h
  src/api/rc_api_editor.c
  src/api/rc_api_info.c
  src/api/rc_api_runtime.c
  src/api/rc_api_user.c
  src/client/rc_client.c
  src/client/rc_client_async.c
  src/client/rc_client_async.h
  src/client/rc_client_do_frame.c
  src/client/rc_client_do_frame.h
  src/client/rc_client_game.c
  src/client/rc_client_game.h
  src/client/rc_client_info.c
  src/client/rc_client_internal.h
  src/client/rc_client_load.c
  src/client/rc_client_load.h
  src/client/rc_client_server.c
  src/client/rc_client_server.h
  src/client/rc_client_user.c
  src/runtime/rc_alloc.c
  src/runtime/rc_alloc.h
  src/runtime/rc_condition.c
  src/runtime/rc_condition.h
  src/runtime/rc_condset.c
  src/runtime/rc_condset.h
  src/runtime/rc_consoleinfo.c
  src/runtime/rc_eval_state.h
  src/runtime/rc_format.c
  src/runtime/rc_format.h
  src/runtime/rc_lboard.c
  src/runtime/rc_lboard.h
  src/runtime/rc_memref.c
  src/runtime/rc_memref.h
  src/runtime/rc_modified_memref.h
  src/runtime/rc_operand.c
  src/runtime/rc_operand.h
  src/runtime/rc_operator.h
  src/runtime/rc_parse_state.h
  src/runtime/rc_richpresence.c
  src/runtime/rc_richpresence.h
  src/runtime/rc_runtime.c
  src/runtime/rc_runtime_progress.c
  src/runtime/rc_trigger.c
  src/runtime/rc_trigger.h
  src/runtime/rc_typed_value.c
  src/runtime/rc_typed_value.h
  src/runtime/rc_validate.c
  src/runtime/rc_validate.h
  src/runtime/rc_value.c
  src/runtime/rc_value.h
  src/util/md5.c
  src/util/md5.h
  src/util/rc_compat.c
  src/util/rc_compat.h
  src/util/rc_util.c
  src/util/rc_version.c
  src/util/rc_version.h
)

target_include_directories(rcheevos PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(rcheevos INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(rcheevos PRIVATE "RCHEEVOS_URL_SSL=1" "RC_NO_THREADS=1")

# RAIntegration is not supported outside of Win32 and only on x64.
if(WIN32 AND CPU_ARCH_X64)
  target_sources(rcheevos PRIVATE
    include/rc_client_raintegration.h
    src/client/rc_client_external.c
    src/client/rc_client_external.h
    src/client/rc_client_external_versions.h
    src/client/rc_client_raintegration.c
    src/client/rc_client_raintegration_internal.h
  )
  target_compile_definitions(rcheevos PUBLIC "RC_CLIENT_SUPPORTS_RAINTEGRATION=1")
endif()
