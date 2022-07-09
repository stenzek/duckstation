add_executable(duckstation-regtest
  regtest_host_display.cpp
  regtest_host_display.h
  regtest_host_interface.cpp
  regtest_host_interface.h
)

target_link_libraries(duckstation-regtest PRIVATE core common frontend-common scmversion)
