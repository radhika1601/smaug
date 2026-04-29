#include <cstring>
#include <iostream>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mpc/mpc.h"

int count102(int *seq, int N) {
  MPC::setNumGates();
  bool s0 = false;
  int c = 0;
  for (int i = 0; i < N; ++i) {
    if (s0 && (seq[i] == 2)) {
      c += 1;
    }
    s0 = (seq[i] == 0) || (s0 && (seq[i] == 1));
  }
  std::cout << "(" << N << ") count102: " << MPC::getNumGates() << "\n";
  return c;
}

int main(int argc, char **argv) {
  time_t t;
  srand((unsigned)time(&t));
  MPC::setup(atoi(argv[1]), atoi(argv[2]));
  int32_t N = atoi(argv[3]);
  int32_t *X = (int32_t *)malloc(sizeof(int32_t) * N);
  memset(X, 0, sizeof(int32_t) * N);
  for (int32_t i = 0; i < N; ++i) {
    X[i] = rand() % 3;
  }
  int32_t a = count102(X, N);

}
