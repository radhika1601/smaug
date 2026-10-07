#include "harness.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

void hdump(const char *file, const char *name, const void *p, int n) {
  FILE *f = fopen(file, "a");
  fprintf(f, "%s %d", name, n);
  for (int i = 0; i < n; ++i)
    fprintf(f, " %d", ((const int32_t *)p)[i]);
  fprintf(f, "\n");
  fclose(f);
}

void hload(const char *file, const char *name, void *p, int n) {
  FILE *f = fopen(file, "r");
  char nm[256];
  int m;
  while (fscanf(f, "%255s %d", nm, &m) == 2) {
    for (int i = 0; i < m; ++i) {
      int v;
      fscanf(f, "%d", &v);
      if (!strcmp(nm, name) && i < n)
        ((int32_t *)p)[i] = v;
    }
  }
  fclose(f);
}
