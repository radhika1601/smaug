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
        "secret_input": {
            "type": "read",
            "size": "N",
            "elementSize": 32
        }
    }
}
```

- `type`: `"read"` (input), `"write"` (output), or `"readwrite"`
- `size`: argument name or constant for array length
- `elementSize`: bit width (32 for int, 8 for bool/char, 64 for long)

### 3. Compile

```bash
# Compile to LLVM IR
clang++ -I/usr/local/include -O0 -Xclang -disable-O0-optnone \
    -S -emit-llvm -std=c++17 my_program.cpp -o my_program.ll

# Strip target triple (required for scalable vector support)
python3 test/remove_target_triple.py my_program.ll

# Run Smaug transformation pipeline
opt --interleave-loops=false \
    -load-pass-plugin=passes/build/libpasses.so \
    -passes=smaug-pipeline \
    --metadata-path=my_program.ll.json \
    --gc=true \
    my_program.ll -o my_program.ll -S

# Link against MPC runtime
clang++ -L/usr/local/lib -lssl -lcrypto -lemp-tool -maes -mssse3 \
    -lgc-opt my_program.ll -std=c++17 -o my_program_mpc
```

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

The metadata JSON maps function names to their secret-shared arguments:

```json
{
    "function_name": {
        "arg_name": {
            "type": "read|write|readwrite",
            "size": "size_arg_name|constant",
            "elementSize": 32
        }
    }
}
```

Multiple arguments can be annotated per function. The `size` field references another function argument by name or uses a numeric constant.
