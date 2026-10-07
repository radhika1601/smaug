# Usage Guide

## Overview

Smaug compiles standard C/C++ programs into MPC protocols through a multi-stage pipeline:

1. **Frontend**: Clang compiles source to LLVM IR
2. **Transformation**: Custom LLVM passes convert the IR to oblivious form and optimize circuits
3. **Linking**: MPC function calls replace private operations
4. **Compilation**: Clang links against the MPC runtime to produce an executable

## Step-by-Step Compilation

### 1. Write Your Program

Write a standard C/C++ program. Secret-shared inputs are passed as pointer arguments:

```cpp
#include "mpc/mpc.h"

void my_function(int *secret_input, int N) {
    MPC::setNumGates();
    // Your computation on secret_input
    printf("Gates: %d\n", MPC::getNumGates());
}

int main(int argc, char **argv) {
    MPC::setup(atoi(argv[1]), atoi(argv[2]));
    int N = atoi(argv[3]);
    int *data = (int *)calloc(N, sizeof(int));

    // Party 1 provides input
    if (MPC::party == 1) {
        for (int i = 0; i < N; i++)
            data[i] = rand() % 100;
    }

    my_function(data, N);
    MPC::finish();
}
```

### 2. Create Metadata

Create a JSON file describing which function arguments are secret-shared:

```json
{
    "my_function": {
        "input": [1, 1, 0, 0],
        "readAccess": {
            "0": 2,
            "1": 3
        },
        "sizes": {
            "0": 32,
            "1": 32
        },
        "output": 1
    }
}
```

- `input`: array of `0`/`1` flags, one per argument in order — `1` means the argument is private/secret-shared
- `readAccess`: maps a pointer argument's index to the index of the argument that holds its array length (omit if no pointer inputs are secret-shared)
- `sizes`: maps a pointer argument's index to its element bit-width (32 for `int`, 8 for `char`/`bool`, 64 for `long`) — only needed for private pointer inputs
- `output`: `1` if the return value is private/secret-shared, `0` otherwise

### 3. Compile

```bash
# LLVM 18 location: /usr/lib/llvm-18 on Ubuntu, $(brew --prefix llvm@18) on macOS
LLVM_PREFIX=/usr/lib/llvm-18

# Compile to LLVM IR
$LLVM_PREFIX/bin/clang++ -I/usr/local/include -O0 -Xclang -disable-O0-optnone \
    -S -emit-llvm -std=c++17 my_program.cpp -o my_program.ll

# Strip target triple (required for scalable vector support)
python3 passes/remove_target_triple.py my_program.ll

# Run Smaug transformation pipeline
$LLVM_PREFIX/bin/opt --interleave-loops=false \
    -load-pass-plugin=passes/build/libpasses.so \
    -passes=smaug-pipeline \
    --metadata-path=my_program.ll.json \
    --gc=true \
    my_program.ll -o my_program.ll -S

# Link against MPC runtime
$LLVM_PREFIX/bin/clang++ -L/usr/local/lib -Wl,-rpath,/usr/local/lib \
    -lssl -lcrypto -lemp-tool -maes -mssse3 \
    -lgc-opt my_program.ll -std=c++17 -o my_program_mpc
```

On macOS with Apple silicon, drop `-maes -mssse3`, and add `-isysroot $(xcrun --show-sdk-path)` and `-L$(brew --prefix openssl@3)/lib` to both `clang++` commands. Also pass `-isysroot` to the first one.

### 4. Run

MPC programs run as two-party protocols. Launch both parties:

```bash
# Terminal 1 (Party 1)
./my_program_mpc 1 12345 16

# Terminal 2 (Party 2)
./my_program_mpc 2 12345 16
```

Arguments: `<party_id> <port> <input_size>`

## Pipeline Variants

| Pipeline | Flag | Backend | Description |
|----------|------|---------|-------------|
| `smaug-pipeline` | `--gc=true` | Yao (garbled circuits) | Default, optimized for boolean circuits |
| `smaug-pipeline` | `--gc=false` | GMW | Boolean GMW  backend |
| `loop-flatten-pipeline` | `--gc=true` | Yao | Additional loop flattening optimization |
| `loop-flatten-pipeline` | `--gc=false` | GMW | loop flattening with GMW backend |
| `no-link-pipeline` | `--gc=true` | - | Transform only, no MPC linking (for debugging) |

## Metadata Format

The metadata JSON maps mangled function names to their privacy annotations:

```json
{
    "function_name": {
        "input": [<arg0_private>, <arg1_private>, ...],
        "readAccess": { "<ptr_arg_index>": <length_arg_index>, ... },
        "sizes": { "<ptr_arg_index>": <element_bits>, ... },
        "output": <return_private>
    }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `input` | array of `0`/`1` | One entry per argument in declaration order; `1` = private |
| `readAccess` | object (optional) | Maps each private pointer argument's index to the index of the argument that holds its array length |
| `sizes` | object (optional) | Maps each private pointer argument's index to its element size in bits |
| `output` | `0` or `1` | `1` if the return value is private/secret-shared |

`readAccess` and `sizes` are only required for functions with private pointer (array) inputs. Functions with no pointer inputs (e.g. `main`) can omit them and set `input` to `[]`.
