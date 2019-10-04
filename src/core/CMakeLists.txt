add_library(core
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

target_include_directories(core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(core Threads::Threads YBaseLib common)

