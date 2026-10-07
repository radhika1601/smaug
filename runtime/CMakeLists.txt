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
    ./src/abi_gmw.cpp
)

add_library(gc SHARED
    ./src/gc.cpp
)

add_library(gc-opt SHARED
    ./src/gc-opt.cpp
    ./src/abi_gc.cpp
)

target_compile_definitions(mpc PRIVATE
    MPC_CIRCUIT_DIR="${CMAKE_INSTALL_PREFIX}/include/mpc/circuits/")

target_link_libraries(mpc PRIVATE OpenSSL::SSL OpenSSL::Crypto ${EMP-TOOL_LIBRARY})
target_link_libraries(gc PRIVATE OpenSSL::SSL OpenSSL::Crypto ${EMP-TOOL_LIBRARY})
target_link_libraries(gc-opt PRIVATE OpenSSL::SSL OpenSSL::Crypto ${EMP-TOOL_LIBRARY})

# Writes the udiv/urem/srem circuits into mpc/circuits (run by hand; the
# output is checked in).
add_executable(gen_circuits ./tools/gen_circuits.cpp)
target_link_libraries(gen_circuits PRIVATE OpenSSL::SSL OpenSSL::Crypto ${EMP-TOOL_LIBRARY})

# ABI tests: one binary per runtime; see tests/run_abi_test.sh.
foreach(rt mpc gc-opt)
    if(rt STREQUAL "mpc")
        set(test_name abi_test_gmw)
    else()
        set(test_name abi_test_gc)
    endif()
    add_executable(${test_name} ./tests/abi_test.cpp)
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${test_name} PRIVATE ${rt} OpenSSL::SSL OpenSSL::Crypto ${EMP-TOOL_LIBRARY})
endforeach()

# Installed libraries find each other and emp-tool through the install prefix.
set_target_properties(mpc gc gc-opt PROPERTIES
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
    INSTALL_NAME_DIR "${CMAKE_INSTALL_PREFIX}/lib")

# Installation
install(DIRECTORY ./mpc/ DESTINATION include/mpc)
install(TARGETS mpc DESTINATION lib)
install(TARGETS gc DESTINATION lib)
install(TARGETS gc-opt DESTINATION lib)

