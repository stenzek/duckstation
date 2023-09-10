# Borrowed from PCSX2.

if(APPLE)
  function(add_metal_sources target sources)
    if(CMAKE_GENERATOR MATCHES "Xcode")
      # If we're generating an xcode project, you can just add the shaders to the main pcsx2 target and xcode will deal with them properly
      # This will make sure xcode supplies code completion, etc (if you use a custom command, it won't)
      set_target_properties(${target} PROPERTIES
        XCODE_ATTRIBUTE_MTL_ENABLE_DEBUG_INFO INCLUDE_SOURCE
      )
      foreach(shader IN LISTS sources)
        target_sources(${target} PRIVATE ${shader})
        set_source_files_properties(${shader} PROPERTIES LANGUAGE METAL)
      endforeach()
    else()
      function(generateMetallib std triple outputName)
        set(MetalShaderOut)
        set(flags
          -ffast-math
          $<$<NOT:$<CONFIG:Release,MinSizeRel>>:-gline-tables-only>
          $<$<NOT:$<CONFIG:Release,MinSizeRel>>:-MO>
        )
        foreach(shader IN LISTS sources)
          file(RELATIVE_PATH relativeShader "${CMAKE_SOURCE_DIR}" "${shader}")
          set(shaderOut ${CMAKE_CURRENT_BINARY_DIR}/${outputName}/${relativeShader}.air)
          list(APPEND MetalShaderOut ${shaderOut})
          get_filename_component(shaderDir ${shaderOut} DIRECTORY)
          add_custom_command(OUTPUT ${shaderOut}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${shaderDir}
            COMMAND xcrun metal ${flags} -std=${std} -target ${triple} -o ${shaderOut} -c ${shader}
            DEPENDS ${shader}
          )
          set(metallib ${CMAKE_CURRENT_BINARY_DIR}/${outputName}.metallib)
        endforeach()
        add_custom_command(OUTPUT ${metallib}
          COMMAND xcrun metallib -o ${metallib} ${MetalShaderOut}
          DEPENDS ${MetalShaderOut}
        )
        target_sources(${target} PRIVATE ${metallib})
        set_source_files_properties(${metallib} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
      endfunction()
      generateMetallib(macos-metal2.0 air64-apple-macos10.13 default)
      generateMetallib(macos-metal2.2 air64-apple-macos10.15 Metal22)
      generateMetallib(macos-metal2.3 air64-apple-macos11.0  Metal23)
    endif()  
  endfunction()
endif()