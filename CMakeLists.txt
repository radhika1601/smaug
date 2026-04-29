cmake_minimum_required(VERSION 3.22)
project(smaug LANGUAGES C CXX)

# Build the MPC runtime libraries (libmpc, libgc, libgc-opt)
add_subdirectory(runtime)

# Build the LLVM pass plugin (libpasses.so)
add_subdirectory(passes)
