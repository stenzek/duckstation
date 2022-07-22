add_subdirectory(fmt)
add_subdirectory(glad)
add_subdirectory(stb)
add_subdirectory(zlib)
add_subdirectory(minizip)
add_subdirectory(lzma)
add_subdirectory(libchdr)
add_subdirectory(xxhash)
add_subdirectory(rapidjson)
add_subdirectory(glslang)
add_subdirectory(imgui)
add_subdirectory(simpleini)

set(LIBSAMPLERATE_ENABLE_SINC_BEST_CONVERTER OFF)
add_subdirectory(libsamplerate)

add_subdirectory(tinyxml2)
add_subdirectory(cubeb)
add_subdirectory(googletest)

if(ENABLE_DISCORD_PRESENCE)
  add_subdirectory(discord-rpc)
endif()

if(ENABLE_CHEEVOS)
  add_subdirectory(rcheevos)
endif()

if(${CPU_ARCH} STREQUAL "aarch32" OR ${CPU_ARCH} STREQUAL "aarch64")
  add_subdirectory(vixl)
endif()

