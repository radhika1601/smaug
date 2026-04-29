#include <cstring>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mpc/mpc.h"

void kmeans(int *X, int *Y, int *clusterX, int *clusterY, int *resX, int *resY,
            int L, int N) {
  MPC::setNumGates();
  int32_t *best = (int32_t *)malloc(sizeof(int32_t) * L);

  for (int i = 0; i < L; ++i) {
    int bestDist = __INT_MAX__;
    for (int c = 0; c < N; ++c) {
      int Xdist = (clusterX[c] - X[i]);
      int Ydist = (clusterY[c] - Y[i]);
      int dist = (Xdist * Xdist) + (Ydist * Ydist);
      if (dist < bestDist) {
        bestDist = dist;
        best[i] = c;
      }
    }
  }

  for (int c = 0; c < N; ++c) {
    int valX = 0, valY = 0, count = 0;
    for (int i = 0; i < L; ++i) {
      if (c == best[i]) {
        valX += X[i];
        valY += Y[i];
        count += 1;
      }
    }

    if (count <= 0) {
      count = 1;
    }

    resX[c] = valX / count;
    resY[c] = valY / count;
  }
  printf("Kmeans: %d\n", MPC::getNumGates());
}

int main(int argc, char **argv) {
  time_t t;
  srand((unsigned)time(&t));
  MPC::setup(atoi(argv[1]), atoi(argv[2]));

  int L = 256, N = atoi(argv[3]);
  int32_t *X = (int32_t *)malloc(sizeof(int32_t) * L);
  memset(X, 0, sizeof(int32_t) * L);
  int32_t *Y = (int32_t *)malloc(sizeof(int32_t) * L);
  memset(Y, 0, sizeof(int32_t) * L);

  int32_t *clusterX = (int32_t *)malloc(sizeof(int32_t) * N);
  memset(clusterX, 0, sizeof(int32_t) * N);
  int32_t *clusterY = (int32_t *)malloc(sizeof(int32_t) * N);
  memset(clusterY, 0, sizeof(int32_t) * N);

  int32_t *resclusterX = (int32_t *)malloc(sizeof(int32_t) * N);
  memset(resclusterX, 0, sizeof(int32_t) * N);
  int32_t *resclusterY = (int32_t *)malloc(sizeof(int32_t) * N);
  memset(resclusterY, 0, sizeof(int32_t) * N);

  if (MPC::party == 1) {
    for (int32_t i = 0; i < L; ++i) {
      X[i] = rand() % 1000;
      Y[i] = rand() % 1000;
    }
    for (int32_t i = 0; i < N; ++i) {
      clusterX[i] = rand() % 1000;
      clusterY[i] = rand() % 1000;
    }
  }

  kmeans(X, Y, clusterX, clusterY, resclusterX, resclusterY, L, N);

  MPC::finish();
}
