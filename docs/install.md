# Installation Guide

## System Requirements

- Ubuntu 22.04 or later (tested on 24.04)
- 8 GB RAM minimum
- CMake 3.22+

## 1. Install LLVM 18

```bash
# Add LLVM apt repository
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 18

# Install development packages
sudo apt install llvm-18-dev clang-18 libclang-18-dev
```

## 2. Install Dependencies

```bash
sudo apt install cmake git build-essential libssl-dev
```

## 3. Install EMP Toolkit

Smaug uses the [EMP toolkit](https://github.com/emp-toolkit) for MPC protocol backends.

```bash
# emp-tool (core library)
git clone https://github.com/emp-toolkit/emp-tool.git
cd emp-tool && cmake -B build && cmake --build build -j$(nproc)
sudo cmake --install build
cd ..

# emp-ot (oblivious transfer)
git clone https://github.com/emp-toolkit/emp-ot.git
cd emp-ot && cmake -B build && cmake --build build -j$(nproc)
sudo cmake --install build
cd ..

# emp-aby (Yao and GMW protocols)
git clone https://github.com/emp-toolkit/emp-aby.git
cd emp-aby && cmake -B build && cmake --build build -j$(nproc)
sudo cmake --install build
cd ..
```

## 4. Build Smaug

```bash
git clone https://github.com/radhika1601/smaug.git
cd smaug

# Build the MPC runtime
cd runtime && mkdir -p build && cd build
cmake .. && make -j$(nproc)
sudo make install
cd ../..

# Build the LLVM pass plugin
cd passes && mkdir -p build && cd build
cmake .. && make -j$(nproc)
cd ../..
```

## 5. Verify Installation

```bash
cd benchmarks
make run
```

All 14 benchmarks should pass.
