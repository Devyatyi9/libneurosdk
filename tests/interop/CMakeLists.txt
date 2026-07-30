cmake_minimum_required(VERSION 3.15)
project(uws_echo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_definitions(-DUWS_NO_SSL)

# uWebSockets headers
set(UWS_DIR "${CMAKE_SOURCE_DIR}/uWebSockets")
set(USOCKETS_DIR "${UWS_DIR}/uSockets")

# uSockets sources (everything except quic)
file(GLOB USOCKETS_SRC "${USOCKETS_DIR}/src/bsd.c"
                       "${USOCKETS_DIR}/src/context.c"
                       "${USOCKETS_DIR}/src/loop.c"
                       "${USOCKETS_DIR}/src/socket.c"
                       "${USOCKETS_DIR}/src/udp.c")

if(WIN32)
    list(APPEND USOCKETS_SRC "${USOCKETS_DIR}/src/eventing/libuv.c")
    find_package(libuv CONFIG QUIET)
    if(NOT libuv_FOUND)
        find_path(LIBUV_INCLUDE_DIR uv.h)
        find_library(LIBUV_LIBRARY uv)
    endif()
else()
    list(APPEND USOCKETS_SRC "${USOCKETS_DIR}/src/eventing/epoll_kqueue.c")
endif()

add_executable(uws_echo uws_echo.cpp ${USOCKETS_SRC})
target_include_directories(uws_echo PRIVATE "${UWS_DIR}/src" "${USOCKETS_DIR}/src")
if(WIN32)
    if(libuv_FOUND)
        target_link_libraries(uws_echo PRIVATE libuv::uv)
    elseif(LIBUV_INCLUDE_DIR)
        target_include_directories(uws_echo PRIVATE "${LIBUV_INCLUDE_DIR}")
        target_link_libraries(uws_echo PRIVATE "${LIBUV_LIBRARY}")
    endif()
endif()
