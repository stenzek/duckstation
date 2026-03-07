cmake_minimum_required(VERSION 3.15)
project(lagoon VERSION 0.0.1)

add_library(lagoon
    lagoon.c

    "${PROJECT_SOURCE_DIR}/include/lagoon.h"
)

target_include_directories(lagoon
PUBLIC
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>

PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

export(PACKAGE lagoon)