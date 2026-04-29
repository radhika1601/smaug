#include <cstring>
#include <iostream>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mpc/mpc.h"

// A => lenA * attA, join at the first column of a and b
int *histogram(int *A, int *B, int length, int *ret, int bins) {
  MPC::setNumGates();
  memset(ret, 0, bins * sizeof(int));
  for (int j = 0; j < bins; ++j) {
    for (int i = 0; i < length; ++i) {
      if (A[i] == j)
        ret[j] += B[i];
    }
  }
  std::cout << "(" << length << "," << bins
            << ") histogram: " << MPC::getNumGates() << "\n";
  return ret;
}

int main(int argc, char **argv) {
  time_t t;
  srand((unsigned)time(&t));
  MPC::setup(atoi(argv[1]), atoi(argv[2]));

  int bins = 5;
  int N = atoi(argv[3]);
  int *X = (int *)malloc(sizeof(int) * N);
  int *Y = (int *)malloc(sizeof(int) * N);
  int *res1 = (int *)malloc(sizeof(int) * bins);
  for (int i = 0; i < N; ++i) {
    X[i] = rand() % bins;
    Y[i] = rand() % 50;
  }

  res1 = histogram(X, Y, N, res1, bins);

  MPC::finish();
}
