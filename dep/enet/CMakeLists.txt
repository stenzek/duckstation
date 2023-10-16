cmake_minimum_required(VERSION 2.8.12...3.20)

project(enet)

# The "configure" step.
include(CheckFunctionExists)
include(CheckStructHasMember)
include(CheckTypeSize)
check_function_exists("fcntl" HAS_FCNTL)
check_function_exists("poll" HAS_POLL)
check_function_exists("getaddrinfo" HAS_GETADDRINFO)
check_function_exists("getnameinfo" HAS_GETNAMEINFO)
check_function_exists("gethostbyname_r" HAS_GETHOSTBYNAME_R)
check_function_exists("gethostbyaddr_r" HAS_GETHOSTBYADDR_R)
check_function_exists("inet_pton" HAS_INET_PTON)
check_function_exists("inet_ntop" HAS_INET_NTOP)
check_struct_has_member("struct msghdr" "msg_flags" "sys/types.h;sys/socket.h" HAS_MSGHDR_FLAGS)
set(CMAKE_EXTRA_INCLUDE_FILES "sys/types.h" "sys/socket.h")
check_type_size("socklen_t" HAS_SOCKLEN_T BUILTIN_TYPES_ONLY)
unset(CMAKE_EXTRA_INCLUDE_FILES)

add_library(enet STATIC)

if(HAS_FCNTL)
    target_compile_definitions(enet PRIVATE -DHAS_FCNTL=1)
endif()
if(HAS_POLL)
    target_compile_definitions(enet PRIVATE -DHAS_POLL=1)
endif()
if(HAS_GETNAMEINFO)
    target_compile_definitions(enet PRIVATE -DHAS_GETNAMEINFO=1)
endif()
if(HAS_GETADDRINFO)
    target_compile_definitions(enet PRIVATE -DHAS_GETADDRINFO=1)
endif()
if(HAS_GETHOSTBYNAME_R)
    target_compile_definitions(enet PRIVATE -DHAS_GETHOSTBYNAME_R=1)
endif()
if(HAS_GETHOSTBYADDR_R)
    target_compile_definitions(enet PRIVATE -DHAS_GETHOSTBYADDR_R=1)
endif()
if(HAS_INET_PTON)
    target_compile_definitions(enet PRIVATE -DHAS_INET_PTON=1)
endif()
if(HAS_INET_NTOP)
    target_compile_definitions(enet PRIVATE -DHAS_INET_NTOP=1)
endif()
if(HAS_MSGHDR_FLAGS)
    target_compile_definitions(enet PRIVATE -DHAS_MSGHDR_FLAGS=1)
endif()
if(HAS_SOCKLEN_T)
    target_compile_definitions(enet PRIVATE -DHAS_SOCKLEN_T=1)
endif()

set(INCLUDE_FILES_PREFIX include/enet)
set(INCLUDE_FILES
    ${INCLUDE_FILES_PREFIX}/callbacks.h
    ${INCLUDE_FILES_PREFIX}/enet.h
    ${INCLUDE_FILES_PREFIX}/list.h
    ${INCLUDE_FILES_PREFIX}/protocol.h
    ${INCLUDE_FILES_PREFIX}/time.h
    ${INCLUDE_FILES_PREFIX}/types.h
    ${INCLUDE_FILES_PREFIX}/unix.h
    ${INCLUDE_FILES_PREFIX}/utility.h
    ${INCLUDE_FILES_PREFIX}/win32.h
)

set(SOURCE_FILES
    callbacks.c
    compress.c
    host.c
    list.c
    packet.c
    peer.c
    protocol.c
    unix.c
    win32.c)

source_group(include FILES ${INCLUDE_FILES})
source_group(source FILES ${SOURCE_FILES})

target_sources(enet PRIVATE
    ${INCLUDE_FILES}
    ${SOURCE_FILES}
)

target_include_directories(enet PUBLIC "${PROJECT_SOURCE_DIR}/include")

