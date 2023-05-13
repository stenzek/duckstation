set(SRCS
  include/enet/callbacks.h
  include/enet/enet.h
  include/enet/list.h
  include/enet/protocol.h
  include/enet/time.h
  include/enet/types.h
  include/enet/unix.h
  include/enet/utility.h
  src/callbacks.c
  src/compress.c
  src/host.c
  src/list.c
  src/packet.c
  src/peer.c
  src/protocol.c
  src/unix.c
)

add_library(enet ${SRCS})
target_include_directories(enet PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(enet INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")