#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <iostream>
#include <cstring>

#include "mpc/mpc.h"

int longest102(int* seq, int N) {
    MPC::setNumGates();
    bool s0    = false;
    int length = 0;
    int maxLen = 0;
    for (int i = 0; i < N; ++i) {
        bool s1 = s0 && (seq[i] == 2);
        s0      = (seq[i] == 0) || (s0 && (seq[i] == 1));
        if (s0 || s1)
            length += 1;
        else
            length = 0;

        if (s1 && (maxLen < length)) {
            maxLen = length;
        }
    }
    std::cout << "(" << N << ") longest102: " << MPC::getNumGates() << "\n";
    return maxLen;
}

int main(int argc, char** argv) {
    time_t t;
  srand((unsigned)time(&t));
  MPC::setup(atoi(argv[1]), atoi(argv[2]));
  int32_t N = atoi(argv[3]);
  int32_t *X = (int32_t *)malloc(sizeof(int32_t) * N);
  memset(X, 0, sizeof(int32_t) * N);
  for (int32_t i = 0; i < N; ++i) {
    X[i] = rand() % 3;
  }
  int32_t a = longest102(X, N);

}
