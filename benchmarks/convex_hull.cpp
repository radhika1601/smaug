#include <cstring>
#include <iostream>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mpc/mpc.h"

void convex_hull(int32_t *X, int32_t *Y, uint N, int32_t *resX, int32_t *resY) {
  MPC::setNumGates();
  for (uint i = 0; i < N; ++i) {
    bool is_hull = true;
    bool cmpP1X = (X[i] <= 0);
    bool cmpP1Y = (Y[i] >= 0);
    bool c = cmpP1X && cmpP1Y;

    if (c) {
      for (uint j = 0; j < N; ++j) {
        bool p2X = X[j] >= X[i];
        bool p2Y = Y[j] <= Y[i];
        bool d = p2X || p2Y;
        if (!d) {
          is_hull = false;
        }
      }
    }

    if (is_hull) {
      resX[i] = X[i];
      resY[i] = Y[i];
    }
  }

  std::cout << N << " convex_hull : " << MPC::getNumGates() << "\n";
}

int main(int argc, char **argv) {
  time_t t;
  srand((unsigned)time(&t));
  MPC::setup(atoi(argv[1]), atoi(argv[2]));
  int32_t N = atoi(argv[3]);
  int32_t *X = (int32_t *)malloc(sizeof(int32_t) * N);
  int32_t *Y = (int32_t *)malloc(sizeof(int32_t) * N);
  int32_t *resX1 = (int32_t *)malloc(sizeof(int32_t) * N);
  int32_t *resY1 = (int32_t *)malloc(sizeof(int32_t) * N);
  memset(resX1, 0, N * sizeof(int32_t));
  memset(resY1, 0, N * sizeof(int32_t));
  if (MPC::party == 1) {
    for (int32_t i = 0; i < N; ++i) {
      X[i] = 50 - (rand() % 100);
      Y[i] = 50 - (rand() % 100);
    }
  }
  convex_hull(X, Y, N, resX1, resY1);

  MPC::finish();
}
