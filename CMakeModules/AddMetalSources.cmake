if(APPLE)
  function(add_metal_sources target sources library_name metal_std)
    set(air_files)
    set(compile_flags -std=${metal_std} -ffast-math)

    foreach(source IN LISTS sources)
      get_filename_component(source_name ${source} NAME)
      set(air_file ${CMAKE_CURRENT_BINARY_DIR}/${library_name}/${source_name}.air)
      list(APPEND air_files ${air_file})

      add_custom_command(
        OUTPUT ${air_file}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/${library_name}
        COMMAND xcrun metal ${compile_flags} -o ${air_file} -c ${source}
        DEPENDS ${source}
        COMMENT "Compiling Metal shader ${source_name}"
      )
    endforeach()

    set(metallib_file ${CMAKE_CURRENT_BINARY_DIR}/${library_name}.metallib)

    add_custom_command(
      OUTPUT ${metallib_file}
      COMMAND xcrun metallib -o ${metallib_file} ${air_files}
      DEPENDS ${air_files}
      COMMENT "Linking Metal library ${library_name}.metallib"
    )

    target_sources(${target} PRIVATE ${metallib_file})
    set_source_files_properties(${metallib_file} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  endfunction()
endif()