cmake_minimum_required(VERSION 3.10)
project(mpc)

set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -Wno-writable-strings -Wno-deprecated-declarations")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer")

set(CMAKE_CXX_STANDARD 17)

find_path(CMAKE_FOLDER NAMES cmake/emp-tool-config.cmake)
include(${CMAKE_FOLDER}/cmake/emp-base.cmake)
find_package(OpenSSL REQUIRED)


find_package(emp-tool REQUIRED)
include_directories(${EMP-TOOL_INCLUDE_DIRS})

find_package(emp-ot REQUIRED)
include_directories(${EMP-OT_INCLUDE_DIRS})

find_package(emp-aby REQUIRED)
include_directories(${EMP-ABY_INCLUDE_DIRS})

add_library(mpc SHARED
    ./src/mpc.cpp
)

add_library(gc SHARED
    ./src/gc.cpp
)

add_library(gc-opt SHARED
    ./src/gc-opt.cpp
)

target_link_libraries(mpc PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(gc PRIVATE OpenSSL::SSL OpenSSL::Crypto emp-tool)
target_link_libraries(gc-opt PRIVATE OpenSSL::SSL OpenSSL::Crypto emp-tool)

# Installation
install(DIRECTORY ./mpc/ DESTINATION include/mpc)
install(TARGETS mpc DESTINATION lib)
install(TARGETS gc DESTINATION lib)
install(TARGETS gc-opt DESTINATION lib)

