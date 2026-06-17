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
# emp-tool (core library), emp-ot (oblivious transfer), emp-sh2pc (semi-honest 2PC GC)
wget -q https://raw.githubusercontent.com/emp-toolkit/emp-readme/master/scripts/install.py
python3 install.py --deps --tool v0.3.x --ot v0.3.x --sh2pc v0.3.x

# emp-aby (Yao and GMW protocols)
git clone -b no-open-fhe https://github.com/radhika1601/ScalableMixedModeMPC.git emp-aby
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
