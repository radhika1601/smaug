#pragma once
// Input and output dumps for the correctness tests. Compiled natively and
// linked into every test program, so the passes never see it.

// Appends "name n v0 v1 ..." to file for n int32 values at p.
void hdump(const char *file, const char *name, const void *p, int n);
// Reads the values recorded under name in file into p (at most n).
void hload(const char *file, const char *name, void *p, int n);
