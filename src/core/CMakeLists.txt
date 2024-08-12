add_library(core
  achievements.cpp
  achievements.h
  achievements_private.h
  analog_controller.cpp
  analog_controller.h
  analog_joystick.cpp
  analog_joystick.h
  bios.cpp
  bios.h
  bus.cpp
  bus.h
  cdrom.cpp
  cdrom.h
  cdrom_async_reader.cpp
  cdrom_async_reader.h
  cheats.cpp
  cheats.h
  controller.cpp
  controller.h
  cpu_code_cache.cpp
  cpu_code_cache.h
  cpu_code_cache_private.h
  cpu_core.cpp
  cpu_core.h
  cpu_core_private.h
  cpu_disasm.cpp
  cpu_disasm.h
  cpu_pgxp.cpp
  cpu_pgxp.h
  cpu_types.cpp
  cpu_types.h
  digital_controller.cpp
  digital_controller.h
  dma.cpp
  dma.h
  fullscreen_ui.cpp
  fullscreen_ui.h
  game_database.cpp
  game_database.h
  game_list.cpp
  game_list.h
  gdb_server.cpp
  gdb_server.h
  gpu.cpp
  gpu.h
  gpu_backend.cpp
  gpu_backend.h
  gpu_commands.cpp
  gpu_hw.cpp
  gpu_hw.h
  gpu_hw_shadergen.cpp
  gpu_hw_shadergen.h
  gpu_shadergen.cpp
  gpu_shadergen.h
  gpu_sw.cpp
  gpu_sw.h
  gpu_sw_backend.cpp
  gpu_sw_backend.h
  gpu_types.h
  guncon.cpp
  guncon.h
  gte.cpp
  gte.h
  gte_types.h
  host.cpp
  host.h
  host_interface_progress_callback.cpp
  host_interface_progress_callback.h
  hotkeys.cpp
  input_types.h
  imgui_overlays.cpp
  imgui_overlays.h
  interrupt_controller.cpp
  interrupt_controller.h
  justifier.cpp
  justifier.h
  mdec.cpp
  mdec.h
  memory_card.cpp
  memory_card.h
  memory_card_image.cpp
  memory_card_image.h
  mips_encoder.h
  multitap.cpp
  multitap.h
  negcon.cpp
  negcon.h
  negcon_rumble.cpp
  negcon_rumble.h
  pad.cpp
  pad.h
  pcdrv.cpp
  pcdrv.h
  pine_server.cpp
  pine_server.h
  playstation_mouse.cpp
  playstation_mouse.h
  psf_loader.cpp
  psf_loader.h
  save_state_version.h
  settings.cpp
  settings.h
  shader_cache_version.h
  sio.cpp
  sio.h
  spu.cpp
  spu.h
  system.cpp
  system.h
  texture_replacements.cpp
  texture_replacements.h
  timers.cpp
  timers.h
  timing_event.cpp
  timing_event.h
  types.h
)

set(RECOMPILER_SRCS
  cpu_recompiler_code_generator.cpp
  cpu_recompiler_code_generator.h
  cpu_recompiler_code_generator_generic.cpp
  cpu_recompiler_register_cache.cpp
  cpu_recompiler_register_cache.h
  cpu_recompiler_thunks.h
  cpu_recompiler_types.h
)

set(NEWREC_SOURCES
  cpu_newrec_compiler.cpp
  cpu_newrec_compiler.h
)

target_precompile_headers(core PRIVATE "pch.h")
target_include_directories(core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(core PUBLIC Threads::Threads common util)
target_link_libraries(core PRIVATE xxhash imgui rapidyaml rcheevos cpuinfo::cpuinfo ZLIB::ZLIB Zstd::Zstd)

if(CPU_ARCH_X64)
  target_compile_definitions(core PUBLIC "ENABLE_RECOMPILER=1" "ENABLE_NEWREC=1" "ENABLE_MMAP_FASTMEM=1")
  target_sources(core PRIVATE ${RECOMPILER_SRCS} ${NEWREC_SOURCES}
    cpu_recompiler_code_generator_x64.cpp
    cpu_newrec_compiler_x64.cpp
    cpu_newrec_compiler_x64.h
  )
  target_link_libraries(core PRIVATE xbyak)
  if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    target_link_libraries(core PRIVATE zydis)
  endif()
  message(STATUS "Building x64 recompiler.")
endif()
if(CPU_ARCH_ARM32)
  target_compile_definitions(core PUBLIC "ENABLE_RECOMPILER=1" "ENABLE_NEWREC=1")
  target_sources(core PRIVATE ${RECOMPILER_SRCS} ${NEWREC_SOURCES}
    cpu_recompiler_code_generator_aarch32.cpp
    cpu_newrec_compiler_aarch32.cpp
    cpu_newrec_compiler_aarch32.h
  )
  target_link_libraries(core PUBLIC vixl)
  message(STATUS "Building AArch32 recompiler.")
endif()
if(CPU_ARCH_ARM64)
  target_compile_definitions(core PUBLIC "ENABLE_RECOMPILER=1" "ENABLE_NEWREC=1" "ENABLE_MMAP_FASTMEM=1")
  target_sources(core PRIVATE ${RECOMPILER_SRCS} ${NEWREC_SOURCES}
    cpu_recompiler_code_generator_aarch64.cpp
    cpu_newrec_compiler_aarch64.cpp
    cpu_newrec_compiler_aarch64.h
  )
  target_link_libraries(core PUBLIC vixl)
  message(STATUS "Building AArch64 recompiler.")
endif()
if(CPU_ARCH_RISCV64)
  target_compile_definitions(core PUBLIC "ENABLE_NEWREC=1" "ENABLE_MMAP_FASTMEM=1")
  target_sources(core PRIVATE ${NEWREC_SOURCES}
    cpu_newrec_compiler_riscv64.cpp
    cpu_newrec_compiler_riscv64.h
  )
  target_link_libraries(core PUBLIC biscuit::biscuit riscv-disas)
  message(STATUS "Building RISC-V 64-bit recompiler.")
endif()

# Copy the provided data directory to the output directory. Borrowed from PCSX2.
function(add_resources target path basedir)
	get_filename_component(dir ${path} DIRECTORY)
	file(RELATIVE_PATH subdir ${basedir} ${dir})
	if(APPLE)
		target_sources(${target} PRIVATE ${path})
		set_source_files_properties(${path} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/${subdir})
	else()
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/resources/${subdir}"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${path}" "$<TARGET_FILE_DIR:${target}>/resources/${subdir}")
	endif()
	source_group(Resources/${subdir} FILES ${path})
endfunction()

function(add_core_resources target)
  add_util_resources(${target})

  if(APPLE)
    # Copy discord-rpc into the bundle
    get_target_property(DISCORD_RPC_LIBRARY DiscordRPC::discord-rpc IMPORTED_LOCATION_RELEASE)
    target_sources(${target} PRIVATE "${DISCORD_RPC_LIBRARY}")
    set_source_files_properties("${DISCORD_RPC_LIBRARY}" PROPERTIES MACOSX_PACKAGE_LOCATION Frameworks)
  endif()

  file(GLOB_RECURSE RESOURCE_FILES ${CMAKE_SOURCE_DIR}/data/resources/*)
  foreach(path IN LISTS RESOURCE_FILES)
    get_filename_component(file ${path} NAME)
    if("${file}" MATCHES "^\\.") # Don't copy macOS garbage (mainly Finder's .DS_Store files) into application
      continue()
    endif()
    add_resources(${target} ${path} ${CMAKE_SOURCE_DIR}/data/resources/)
  endforeach()
  if(ALLOW_INSTALL)
    install_imported_dep_library(cpuinfo::cpuinfo)
    install(DIRECTORY "$<TARGET_FILE_DIR:${target}>/resources" DESTINATION "${CMAKE_INSTALL_PREFIX}")
  endif()
endfunction()
