# Smaug

**Modular Augmentation of LLVM for Secure Multi-Party Computation**

Smaug is an LLVM-based compiler that transforms standard C/C++ and Rust programs into secure multi-party computation (MPC) protocols. It automatically converts non-oblivious programs into their oblivious counterparts and applies circuit-level optimizations as LLVM code transformations.

## Features

- **Language support**: Compile from C, C++, and Rust via standard LLVM frontends
- **Automatic oblivious transformation**: Convert programs with private control flow to trace-oblivious form without programmer annotations
- **Circuit optimization**: Loop vectorization, loop flattening, loop splitting, and reduction parallelization to minimize circuit depth
- **Multi-protocol backends**: Supports Yao's garbled circuits and GMW protocol via the EMP toolkit
- **LLVM integration**: Leverages LLVM's optimization pipeline, error messaging, and tooling

## Prerequisites

- Ubuntu 22.04+ (tested on 24.04)
- LLVM 18 (`apt install llvm-18 clang-18`)
- CMake 3.22+
- OpenSSL development libraries
- [EMP toolkit](https://github.com/emp-toolkit): emp-tool, emp-ot, emp-sh2pc, [emp-aby](https://github.com/radhika1601/ScalableMixedModeMPC)

See [docs/install.md](docs/install.md) for detailed installation instructions.

## Building

```bash
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

## Running Benchmarks

```bash
cd benchmarks

# Garbled circuit (Yao) backend
make run

# GMW backend
make run-no-gc

# With loop flattening optimization
make run-loop-flatten
make run-loop-flatten-no-gc
```

## Benchmarks

| Program | Description | Input Size |
|---------|-------------|------------|
| biometric | Biometric matching | N=4096, D=4 |
| convex_hull | Convex hull computation | N=256 |
| count10/count102 | Pattern counting | N=4096 |
| db_variance | Database variance | N=256 |
| histogram | Histogram computation | N=4096, D=5 |
| inner_product | Inner product | N=4096 |
| kmeans_iteration | K-means clustering | N=256, k=16 |
| longest102 | Longest pattern | N=4096 |
| matmul | Matrix multiplication | N=16 |
| max_dist_between_syms | Maximum distance | N=4096 |
| minimal_points | Minimal points | N=256 |
| mnistRelu | MNIST with ReLU | N=16 |
| psi | Private set intersection | N=1024 |

## Compiling Your Own Programs

See [docs/usage.md](docs/usage.md) for a step-by-step guide on compiling MPC programs with Smaug.

## Project Structure

```
smaug/
  passes/       LLVM transformation passes (libpasses.so)
  runtime/      MPC runtime libraries (libgc-opt, libmpc)
  benchmarks/   Benchmark programs and test pipeline
  test/         Test utilities
  docs/         Documentation
```

## Citation

If you use Smaug in your research, please cite:

```bibtex
@INPROCEEDINGS{11023285,
  author={Garg, Radhika and Wang, Xiao},
  booktitle={2025 IEEE Symposium on Security and Privacy (SP)}, 
  title={Smaug: Modular Augmentation of LLVM for MPC}, 
  year={2025}}
```

## License

MIT License. See [LICENSE](LICENSE).
