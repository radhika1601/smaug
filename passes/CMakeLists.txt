cmake_minimum_required(VERSION 3.22)

if(APPLE)
    # CMAKE_HOST_SYSTEM_PROCESSOR is not set before project(), so test the prefix.
    if(EXISTS "/opt/homebrew/opt/llvm@18")
        set(LLVM_18_DEFAULT_PREFIX "/opt/homebrew/opt/llvm@18")
    else()
        set(LLVM_18_DEFAULT_PREFIX "/usr/local/opt/llvm@18")
    endif()
else()
    set(LLVM_18_DEFAULT_PREFIX "/usr/lib/llvm-18")
endif()
set(LLVM_18_PREFIX "${LLVM_18_DEFAULT_PREFIX}" CACHE PATH "Path to the LLVM 18 install")

# The compiler must be selected before project() to affect the initial CMake
# configure. If the cache already contains a compiler, the user must reconfigure
# from a clean build directory for the change to take effect.
if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "${LLVM_18_PREFIX}/bin/clang" CACHE FILEPATH "C compiler" FORCE)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "${LLVM_18_PREFIX}/bin/clang++" CACHE FILEPATH "C++ compiler" FORCE)
endif()

project(passes LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

find_package(LLVM REQUIRED CONFIG HINTS "${LLVM_18_PREFIX}/lib/cmake/llvm")
add_definitions(${LLVM_DEFINITIONS})

message(STATUS "LLVM package version: ${LLVM_PACKAGE_VERSION}")
message(STATUS "LLVM include dirs: ${LLVM_INCLUDE_DIRS}")
message(STATUS "LLVM library dirs: ${LLVM_LIBRARY_DIRS}")
message(STATUS "CMake build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C compiler: ${CMAKE_C_COMPILER}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")

if(DEFINED LLVM_TOOLS_BINARY_DIR AND EXISTS "${LLVM_TOOLS_BINARY_DIR}/llvm-config")
    execute_process(
        COMMAND "${LLVM_TOOLS_BINARY_DIR}/llvm-config" --build-mode --assertion-mode
        OUTPUT_VARIABLE LLVM_CONFIG_MODES
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    string(REPLACE "\n" " " LLVM_CONFIG_MODES "${LLVM_CONFIG_MODES}")
    message(STATUS "llvm-config modes: ${LLVM_CONFIG_MODES}")

    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND LLVM_CONFIG_MODES MATCHES "OFF")
        message(WARNING
            "You are building this pass with CMAKE_BUILD_TYPE=Debug against a system LLVM "
            "install that has assertions disabled. That is fine for source-level debugging "
            "of this plugin, but it is not equivalent to a full debug LLVM build. If you "
            "need debug assertions or exact behavior parity with LLVM internals, rebuild LLVM "
            "from source with Debug or RelWithDebInfo and LLVM_ENABLE_ASSERTIONS=ON. For the "
            "system package, prefer RelWithDebInfo for the plugin."
        )
    endif()
endif()

include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})

include_directories(./include)
include_directories(./include/Vectorize)
include_directories(./src)
# The runtime ABI list (mpc/smaug_abi.def) is shared with the runtime.
include_directories(../runtime)

add_library(passes MODULE
    ./src/MPCHierarchical.cpp
    ./src/TopologicalTraversal.cpp
    ./src/Metadata.cpp
    ./src/MetadataHelper.cpp
    ./src/Passes.cpp
    ./src/MPCLink.cpp
    ./src/Options.cpp
    ./src/lower/Abi.cpp
    ./src/lower/BoolNarrowing.cpp
    ./src/lower/Lowering.cpp
    ./src/lower/MPCLower.cpp
    ./src/lower/SecretAnalysis.cpp
    ./src/lower/SecretSpec.cpp
    ./src/lower/VectorLoops.cpp
    ./src/MPCLoopFlatten.cpp
    ./src/MPCLoopMemAlign.cpp
    ./src/MPCLoopReconstruct.cpp
    ./src/MPCRemoveOps.cpp
    ./src/ParallelizeReductions.cpp
    ./src/VectorMPCLink.cpp
    ./src/VectorizeHelper.cpp
    ./src/Vectorize/LoopVectorize.cpp
    ./src/Vectorize/LoopVectorizationLegality.cpp
    ./src/Vectorize/VPlan.cpp
    ./src/Vectorize/VPlanAnalysis.cpp
    ./src/Vectorize/VPlanHCFGBuilder.cpp
    ./src/Vectorize/VPlanRecipes.cpp
    ./src/Vectorize/VPlanSLP.cpp
    ./src/Vectorize/VPlanTransforms.cpp
    ./src/Vectorize/VPlanVerifier.cpp
)

target_compile_options(passes PRIVATE -fno-exceptions -fno-rtti -g)
if(APPLE)
    # LLVM symbols are resolved from the host opt at load time.
    target_link_options(passes PRIVATE "-Wl,-undefined,dynamic_lookup")
else()
    target_link_options(passes PRIVATE "-Wl,-Bsymbolic-functions")
endif()

message(STATUS "CMAKE_C_FLAGS: ${CMAKE_C_FLAGS}")
message(STATUS "CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
