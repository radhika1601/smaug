#include <cstring>
#include <iostream>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mpc/mpc.h"

void mnist_relu(int *X, int N, int *resX) {
  MPC::setNumGates();
  for (int i = 0; i < N; ++i) {
    if (X[i] > 1) {
      resX[i] = 1;
    }
  }
  std::cout << "(" << N << ") mnist_relu: " << MPC::getNumGates() << "\n";
}

int main(int argc, char **argv) {
  time_t t;
  srand((unsigned)time(&t));
  MPC::setup(atoi(argv[1]), atoi(argv[2]));

  int32_t N = atoi(argv[3]);
  int32_t *X = (int32_t *)malloc(sizeof(int32_t) * N);
  int32_t *resX = (int32_t *)malloc(sizeof(int32_t) * N);
  memset(resX, 0, sizeof(int32_t) * N);
  for (int32_t i = 0; i < N; ++i) {
    X[i] = rand();
  }

  mnist_relu(X, N, resX);

  MPC::finish();
}
