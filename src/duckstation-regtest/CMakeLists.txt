add_executable(duckstation-regtest
  regtest_host.cpp
)

target_include_directories(duckstation-regtest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(duckstation-regtest PRIVATE core common scmversion)

add_core_resources(duckstation-regtest)