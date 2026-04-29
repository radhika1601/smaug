#include <cstring>
#include <iostream>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mpc/mpc.h"

int matmul(int *seq, int N, int sym) {
  int dist = 0, maxDist = 0;
  MPC::setNumGates();
  for (int i = 0; i < N; ++i) {
    if (seq[i] != sym) {
      dist += 1;
    } else {
      if (dist > maxDist)
        maxDist = dist;
      dist = 0;
    }
  }
  std::cout << "(" << N << ") matmul: " << MPC::getNumGates()
            << "\n";

  return maxDist;
}

int main(int argc, char **argv) {
  time_t t;
  srand((unsigned)time(&t));
  MPC::setup(atoi(argv[1]), atoi(argv[2]));
  int32_t N = atoi(argv[3]);
  int32_t *X = (int32_t *)malloc(sizeof(int32_t) * N * N);
  int32_t *y = (int32_t *)malloc(sizeof(int32_t) * N);
  int32_t *z = (int32_t *)malloc(sizeof(int32_t) * N);
  for (int32_t i = 0; i < N; ++i) {
    X[i] = rand() % 10;
  }
  int32_t a = matmul(X, N, 0);

}
