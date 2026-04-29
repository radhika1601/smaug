#include <cstring>
#include <iostream>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mpc/mpc.h"

void psi(int *X, int *Y, int N, int *resX) {
  MPC::setNumGates();
  for (int i = 0; i < N; ++i) {
    bool found = false;
    for (int j = 0; j < N; ++j) {
      if (X[i] == Y[j])
        found = true;
    }

    if (found)
      resX[i] = X[i];
  }
  std::cout << "(" << N << ") psi: " << MPC::getNumGates() << "\n";
}

int main(int argc, char **argv) {
  time_t t;
  srand((unsigned)time(&t));
  MPC::setup(atoi(argv[1]), atoi(argv[2]));

  int32_t N = atoi(argv[3]);

  int32_t *X = (int32_t *)malloc(sizeof(int32_t) * N);
  int32_t *Y = (int32_t *)malloc(sizeof(int32_t) * N);
  int32_t *resX = (int32_t *)malloc(sizeof(int32_t) * N);
  memset(X, 0, sizeof(int32_t) * N);
  memset(Y, 0, sizeof(int32_t) * N);
  memset(resX, 0, sizeof(int32_t) * N);
  for (int32_t i = 0; i < N; ++i) {
    X[i] = rand() % 1000;
    Y[i] = rand() % 1000;
  }

  psi(X, Y, N, resX);

  MPC::finish();
}
