#include <cstring>
#include <iostream>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mpc/mpc.h"

int inner_product(int *A, int *B, int length) {
  int res = 0;
  MPC::setNumGates();
  for (int i = 0; i < length; ++i) {
    res += (A[i] * B[i]);
  }
  std::cout << "(" << length << ") inner_product: " << MPC::getNumGates()
            << "\n";
  return res;
}

int main(int argc, char **argv) {
  time_t t;
  srand((unsigned)time(&t));
  MPC::setup(atoi(argv[1]), atoi(argv[2]));

  int length = atoi(argv[3]);

  int *A = (int *)malloc(sizeof(int) * length);
  int *B = (int *)malloc(sizeof(int) * length);
  for (int i = 0; i < length; ++i) {
    A[i] = rand() % 50;
    B[i] = rand() % 50;
  }

  int res = inner_product(A, B, length);

}
