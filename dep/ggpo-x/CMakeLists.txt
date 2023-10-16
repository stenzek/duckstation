add_library(ggpo-x
  include/ggponet.h
  src/backends/backend.h
  src/backends/p2p.cpp
  src/backends/p2p.h
  src/backends/spectator.cpp
  src/backends/spectator.h
  src/backends/synctest.h
  src/backends/synctest.cpp
  src/bitvector.h
  src/bitvector.cpp
  src/game_input.h
  src/game_input.cpp
  src/input_queue.h
  src/input_queue.cpp
  src/log.cpp
  src/log.h
  src/main.cpp
  src/network/udp_msg.h
  src/network/udp_proto.cpp
  src/network/udp_proto.h
  src/platform.cpp
  src/platform.h
  src/ring_buffer.h
  src/static_buffer.h
  src/sync.cpp
  src/sync.h
  src/timesync.cpp
  src/timesync.h
  src/types.h
)

target_include_directories(ggpo-x PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_include_directories(ggpo-x INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(ggpo-x PRIVATE enet)

