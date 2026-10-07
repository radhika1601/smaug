// Plaintext stand-ins for the MPC runtime calls a benchmark makes itself.
// Linked into the native reference build, which runs without the passes.
#include <cstdint>

namespace MPC {
int party = 1;
int getNumGates() { return 0; }
void setNumGates() {}
void setup(int p, int) { party = p; }
void finish() {}

// Fills arr with element[0], plus i when consecutive is set.
void store(void *arr, void *element, int n, int elementSize, bool,
           bool consecutive) {
  for (int i = 0; i < n; ++i) {
    int64_t add = consecutive ? i : 0;
    if (elementSize == 1)
      ((int8_t *)arr)[i] = ((int8_t *)element)[0] + add;
    else if (elementSize == 2)
      ((int16_t *)arr)[i] = ((int16_t *)element)[0] + add;
    else if (elementSize == 4)
      ((int32_t *)arr)[i] = ((int32_t *)element)[0] + add;
    else if (elementSize == 8)
      ((int64_t *)arr)[i] = ((int64_t *)element)[0] + add;
  }
}
} // namespace MPC
