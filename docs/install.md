# Installation Guide

## System Requirements

- Ubuntu 22.04 or later (tested on 24.04), or macOS with Homebrew and Xcode command line tools
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

On macOS:

```bash
xcode-select --install
brew install llvm@18
```

Homebrew installs LLVM 18 under `/opt/homebrew/opt/llvm@18` (`/usr/local/opt/llvm@18` on Intel).

## 2. Install Dependencies

```bash
sudo apt install cmake git build-essential libssl-dev
```

On macOS:

```bash
brew install cmake git openssl@3 wget
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

Smaug requires the v0.3.x branches. It does not build against emp master.

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

## Local prefix (macOS, or a machine with another EMP version)

The steps above install into `/usr/local`. Use a local prefix instead when `/usr/local` already holds a different EMP version, or on macOS, where `install.py --deps` uses apt. The benchmarks Makefile uses `smaug/.deps` automatically when it exists. No step needs `sudo`.

```bash
cd smaug
PREFIX=$PWD/.deps
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
CMAKE_ARGS="-DCMAKE_INSTALL_PREFIX=$PREFIX -DCMAKE_PREFIX_PATH=$PREFIX -DCMAKE_POLICY_VERSION_MINIMUM=3.5"
# macOS only
CMAKE_ARGS="$CMAKE_ARGS -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)"

mkdir -p $PREFIX/src && cd $PREFIX/src
for repo in emp-tool emp-ot emp-sh2pc; do
    git clone -b v0.3.x https://github.com/emp-toolkit/$repo.git
    (cd $repo && cmake -B build $CMAKE_ARGS && cmake --build build -j$JOBS && cmake --install build)
done
git clone -b no-open-fhe https://github.com/radhika1601/ScalableMixedModeMPC.git emp-aby
(cd emp-aby && cmake -B build $CMAKE_ARGS && cmake --build build -j$JOBS && cmake --install build)
cd ../..

# MPC runtime
(cd runtime && cmake -B build $CMAKE_ARGS && cmake --build build -j$JOBS && cmake --install build)

# LLVM pass plugin
(cd passes && cmake -B build && cmake --build build -j$JOBS)
```

`CMAKE_POLICY_VERSION_MINIMUM` is needed with CMake 4, because emp-aby declares an old minimum version.

On macOS the pass plugin uses Homebrew LLVM 18 at `/opt/homebrew/opt/llvm@18`. Pass `-DLLVM_18_PREFIX=<path>` to override it. Delete any `passes/build` directory configured with a different compiler first.

To use a different prefix with the benchmarks, run `make run SMAUG_PREFIX=<path>`.

## 5. Verify Installation

```bash
cd benchmarks
make run
```

All 13 benchmarks should pass. This checks only that both parties exit cleanly. To check the computed values, run:

```bash
make -C tests check
```
