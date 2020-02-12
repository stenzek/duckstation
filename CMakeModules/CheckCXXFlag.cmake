include(CheckCXXCompilerFlag)

function(check_cxx_flag flag var)
    CHECK_CXX_COMPILER_FLAG("-Werror ${flag}" ${var})
    if(${var})
       set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${flag}" PARENT_SCOPE)
    endif()
endfunction()
