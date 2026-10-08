#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>

#include "mpc/mpc.h"

// Scalar test: secret branch with a return in each arm
int32_t op(int32_t a, int32_t b) {
  MPC::setNumGates();
  if (a > b)
    return a - b;
  return b - a;
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
