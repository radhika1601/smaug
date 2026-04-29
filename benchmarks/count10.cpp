#include <cstring>
#include <iostream>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mpc/mpc.h"

int32_t count10(int32_t *seq, int32_t N) {
  MPC::setNumGates();
  bool s0 = false, s1 = false;
  int32_t c = 0;
  for (int32_t i = 0; i < N; ++i) {
    bool b1 = (seq[i] == 0);
    bool b2 = (seq[i] == 1);
    b1 = s1 && (seq[i] == 0);
    if (s1 && (seq[i] == 0)) {
      c += 1;
    }
    s1 = (seq[i] == 0) && (s0 || s1);
    s0 = (seq[i] == 1);
  }
  std::cout << "(" << N << ") count10: " << MPC::getNumGates() << "\n";
  return c;
}

int main(int argc, char **argv) {
  time_t t;
  srand((unsigned)time(&t));
  MPC::setup(atoi(argv[1]), atoi(argv[2]));
  int32_t N = atoi(argv[3]);
  int32_t *X = (int32_t *)malloc(sizeof(int32_t) * N);
  memset(X, 0, sizeof(int32_t) * N);
  for (int32_t i = 0; i < N; ++i) {
    X[i] = rand() % 3;
  }

  int32_t a = count10(X, N);

}
