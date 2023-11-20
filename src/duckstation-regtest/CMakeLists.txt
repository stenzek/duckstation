add_executable(duckstation-regtest
  regtest_host.cpp
)

target_link_libraries(duckstation-regtest PRIVATE core common scmversion)

add_core_resources(duckstation-regtest)