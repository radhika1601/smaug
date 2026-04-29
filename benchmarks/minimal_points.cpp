#include <cstring>
#include <iostream>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mpc/mpc.h"

void minimal_points(int *X, int *Y, int N, int *resX, int *resY) {
  MPC::setNumGates();
  for (int i = 0; i < N; ++i) {
    bool bx = false;
    for (int j = 0; j < N; ++j) {
      bx = bx || ((X[j] < X[i]) && (Y[j] < Y[i]));
    }

    if (!bx) {
      resX[i] = X[i];
      resY[i] = Y[i];
    }
  }
  std::cout << "(" << N << ") minimal_points: " << MPC::getNumGates() << "\n";
}

int main(int argc, char **argv) {
  time_t t;
  srand((unsigned)time(&t));
  MPC::setup(atoi(argv[1]), atoi(argv[2]));

  int32_t N = atoi(argv[3]);
  int32_t *X = (int32_t *)malloc(sizeof(int32_t) * N);
  int32_t *Y = (int32_t *)malloc(sizeof(int32_t) * N);

  int32_t *resX = (int32_t *)malloc(sizeof(int32_t) * N);
  int32_t *resY = (int32_t *)malloc(sizeof(int32_t) * N);

  memset(resX, 0, sizeof(int32_t) * N);
  memset(resY, 0, sizeof(int32_t) * N);

  for (int32_t i = 0; i < N; ++i) {
    X[i] = rand() % 100;
    Y[i] = rand() % 100;
  }

  minimal_points(X, Y, N, resX, resY);
  MPC::finish();
}
