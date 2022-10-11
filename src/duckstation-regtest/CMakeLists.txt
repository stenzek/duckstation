add_executable(duckstation-regtest
  regtest_host_display.cpp
  regtest_host_display.h
  regtest_host.cpp
)

target_link_libraries(duckstation-regtest PRIVATE core common frontend-common scmversion)
