#include "mpc/mpc.h"
#include <cstring>
#include <iostream>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int biometric(int *a, int *b, int nd, int D) {
  int N = nd / D;
  MPC::setNumGates();
  int min_idx = 0, min_dist = __INT_MAX__;
  for (int i = 0; i < N; ++i) {
    int diff = 0;
    for (int j = 0; j < D; ++j) {
      diff += (a[i * D + j] - b[j]) * (a[i * D + j] - b[j]);
    }
    if (min_dist > diff) {
      min_dist = diff;
      min_idx = i;
    }
  }
  std::cout << "(N, D) biometric: " << MPC::getNumGates() << "\n";
  return min_idx;
}

int main(int argc, char **argv) {
  time_t t;
  srand((unsigned)time(&t));
  MPC::setup(atoi(argv[1]), atoi(argv[2]));
  int32_t N = atoi(argv[3]);
  int D = 4;
  int *a = (int *)malloc(sizeof(int *) * N * D);
  memset(a, 0, sizeof(int) * N * D);
  int *b = (int *)malloc(sizeof(int) * D);
  memset(b, 0, sizeof(int) * D);

  for (int i = 0; i < D; ++i) {
    b[i] = rand() % 50;
    for (int k = 0; k < N; ++k) {
      a[i + k * D] = rand() % 50;
    }
  }

  int v1 = biometric(a, b, N*D, D);
  MPC::finish();
}
