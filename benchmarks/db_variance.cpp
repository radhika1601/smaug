#include <cstring>
#include <iostream>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mpc/mpc.h"

int db_variance(int *A, int length) {
  MPC::setNumGates();
  int sum = 0;
  for (int i = 0; i < length; ++i)
    sum = sum + A[i];

  int exp = sum / length;
  int res = 0;
  for (int i = 0; i < length; ++i) {
    int dist = A[i] - exp;
    res += dist * dist;
  }

  int var = res / length;
  std::cout << "(" << length << ") db_variance: " << MPC::getNumGates() << "\n";

  return var;
}

int main(int argc, char **argv) {
  time_t t;
  srand((unsigned)time(&t));
  MPC::setup(atoi(argv[1]), atoi(argv[2]));

  int length = atoi(argv[3]);

  int *A = (int *)malloc(sizeof(int) * length);
  for (int i = 0; i < length; ++i) {
    A[i] = rand();
  }

  int res = db_variance(A, length);

  MPC::finish();
}
