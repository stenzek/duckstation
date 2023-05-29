# Main library

add_library(biscuit
    # Source files
    assembler.cpp
    assembler_crypto.cpp
    assembler_vector.cpp
    code_buffer.cpp
    cpuinfo.cpp

    # Headers
    "${PROJECT_SOURCE_DIR}/include/biscuit/assembler.hpp"
    "${PROJECT_SOURCE_DIR}/include/biscuit/assert.hpp"
    "${PROJECT_SOURCE_DIR}/include/biscuit/code_buffer.hpp"
    "${PROJECT_SOURCE_DIR}/include/biscuit/csr.hpp"
    "${PROJECT_SOURCE_DIR}/include/biscuit/isa.hpp"
    "${PROJECT_SOURCE_DIR}/include/biscuit/label.hpp"
    "${PROJECT_SOURCE_DIR}/include/biscuit/registers.hpp"
    "${PROJECT_SOURCE_DIR}/include/biscuit/vector.hpp"
    "${PROJECT_SOURCE_DIR}/include/biscuit/cpuinfo.hpp"
)
add_library(biscuit::biscuit ALIAS biscuit)

target_include_directories(biscuit
PUBLIC
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>

PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(biscuit
PRIVATE
    cxx_std_20
)

if (MSVC)
    target_compile_options(biscuit
    PRIVATE
        /MP
        /Zi
        /Zo
        /permissive-
        /EHsc
        /utf-8
        /volatile:iso
        /Zc:externConstexpr
        /Zc:inline
        /Zc:throwingNew

        # Warnings
        /W4
        /we4062 # enumerator 'identifier' in a switch of enum 'enumeration' is not handled
        /we4101 # 'identifier': unreferenced local variable
        /we4265 # 'class': class has virtual functions, but destructor is not virtual
        /we4287 # 'operator' : unsigned/negative constant mismatch
        /we4365 # 'action' : conversion from 'type_1' to 'type_2', signed/unsigned mismatch
        /we4388 # signed/unsigned mismatch
        /we4547 # 'operator' : operator before comma has no effect; expected operator with side-effect
        /we4549 # 'operator1': operator before comma has no effect; did you intend 'operator2'?
        /we4555 # Expression has no effect; expected expression with side-effect
        /we4715 # 'function': not all control paths return a value
        /we4834 # Discarding return value of function with 'nodiscard' attribute
        /we5038 # data member 'member1' will be initialized after data member 'member2'
    )
elseif (("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang") OR ("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU"))
    target_compile_options(biscuit
    PRIVATE
        -Wall
        -Wextra
        -Wconversion
        -Wsign-conversion

        -Werror=array-bounds
        -Werror=cast-qual
        -Werror=ignored-qualifiers
        -Werror=implicit-fallthrough
        -Werror=sign-compare
        -Werror=reorder
        -Werror=uninitialized
        -Werror=unused-function
        -Werror=unused-result
        -Werror=unused-variable
    )
endif()

if (BISCUIT_CODE_BUFFER_MMAP)
    target_compile_definitions(biscuit
    PRIVATE
        -DBISCUIT_CODE_BUFFER_MMAP
    )
endif()

# Install target

include(GNUInstallDirs)
set(BISCUIT_INSTALL_CONFIGDIR "${CMAKE_INSTALL_LIBDIR}/cmake/biscuit")

# Set install target and relevant includes.
install(TARGETS biscuit
    EXPORT biscuit-targets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(
    DIRECTORY   "${PROJECT_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

# Export targets to a script
install(EXPORT biscuit-targets
    FILE
        biscuit-targets.cmake
    NAMESPACE
        biscuit::
    DESTINATION
        "${BISCUIT_INSTALL_CONFIGDIR}"
)

# Now create the config version script
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/biscuit-config-version.cmake"
    VERSION
        ${PROJECT_VERSION}
    COMPATIBILITY
        SameMajorVersion
)

configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/biscuit-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/biscuit-config.cmake"

    INSTALL_DESTINATION "${BISCUIT_INSTALL_CONFIGDIR}"
)

# Now install the config and version files.
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/biscuit-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/biscuit-config-version.cmake"

    DESTINATION "${BISCUIT_INSTALL_CONFIGDIR}"
)

# Export library from the build tree.
export(EXPORT biscuit-targets
    FILE
        "${CMAKE_CURRENT_BINARY_DIR}/biscuit-targets.cmake"
    NAMESPACE
        biscuit::
)
export(PACKAGE biscuit)
