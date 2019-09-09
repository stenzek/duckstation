add_library(pse
    cpu_bus.cpp
    cpu_bus.h
    cpu_bus.inl
    cpu_core.cpp
    cpu_core.h
    cpu_core.inl
    system.cpp
    system.h
    types.h
)

target_include_directories(pse PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(pse PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(pse Threads::Threads YBaseLib common)

