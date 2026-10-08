#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>

#include "mpc/mpc.h"

// Scalar test: loop with a public bound over a secret scalar
int32_t op(int32_t a, int32_t b) {
  MPC::setNumGates();
  int32_t c = 0;
  for (int i = 0; i < 32; ++i)
    c += ((a ^ b) >> i) & 1;
  return c;
}

int main(int argc, char **argv) {
  time_t t;
  srand((unsigned)time(&t));
  MPC::setup(atoi(argv[1]), atoi(argv[2]));
  int32_t a = rand() % 201 - 100;
  int32_t b = rand() % 201 - 100;
  int32_t res = op(a, b);
  MPC::finish();
}
