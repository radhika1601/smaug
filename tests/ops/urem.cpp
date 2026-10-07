#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>

#include "mpc/mpc.h"

// Operation test: elementwise unsigned remainder
void op(int32_t *a, int32_t *b, int32_t *out, int N) {
  MPC::setNumGates();
  for (int i = 0; i < N; ++i)
    out[i] = (uint32_t)a[i] % ((uint32_t)b[i] | 1u);
  std::cout << "(" << N << ") op: " << MPC::getNumGates() << "\n";
}

int main(int argc, char **argv) {
  time_t t;
  srand((unsigned)time(&t));
  MPC::setup(atoi(argv[1]), atoi(argv[2]));
  int N = atoi(argv[3]);
  int32_t *a = (int32_t *)malloc(sizeof(int32_t) * N);
  int32_t *b = (int32_t *)malloc(sizeof(int32_t) * N);
  int32_t *out = (int32_t *)malloc(sizeof(int32_t) * N);
  memset(out, 0, sizeof(int32_t) * N);
  for (int i = 0; i < N; ++i) {
    a[i] = rand() % 201 - 100;
    b[i] = rand() % 201 - 100;
  }
  op(a, b, out, N);
  MPC::finish();
}
