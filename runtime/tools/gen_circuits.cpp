// Writes Bristol circuits for the GMW runtime: udiv, urem and srem for 8,
// 16, 32 and 64 bits, and float32 add, sub, mult and div. The float circuits
// replace older files that computed wrong results; these follow emp::Float,
// which the GC runtime uses too.
//
//   gen_circuits <output dir>
//
// Each circuit takes a (ALICE, w bits) and b (BOB, w bits) and outputs w
// bits, in the same format as the other files in runtime/mpc/circuits.

#include "emp-tool/emp-tool.h"
#include "emp-tool/execution/plain_prot.h"

#include <cstdio>
#include <functional>
#include <string>

using namespace emp;

namespace {

// Unsigned division and remainder through emp's signed operations: one extra
// zero bit makes both operands non-negative.
Integer unsignedOp(const Integer &a, const Integer &b, bool rem) {
  Integer x = a, y = b;
  x.resize(a.size() + 1, false);
  y.resize(b.size() + 1, false);
  Integer r = rem ? x % y : x / y;
  r.resize(a.size(), false);
  return r;
}

Float toFloat(const Integer &x) {
  Float f;
  for (int i = 0; i < 32; ++i)
    f.value[i] = x[i];
  return f;
}

Integer fromFloat(const Float &f) {
  return Integer(std::vector<Bit>(f.value.begin(), f.value.end()));
}

void write(const std::string &path, int w,
           const std::function<Integer(const Integer &, const Integer &)> &f) {
  setup_plain_prot(true, path);
  Integer a(w, 0, ALICE), b(w, 0, BOB);
  Integer r = f(a, b);
  r.reveal<std::string>(PUBLIC);
  finalize_plain_prot();
  printf("wrote %s\n", path.c_str());
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <output dir>\n", argv[0]);
    return 2;
  }
  std::string dir = argv[1];
  for (int w : {8, 16, 32, 64}) {
    std::string s = std::to_string(w);
    write(dir + "/udiv" + s + ".txt", w,
          [](const Integer &a, const Integer &b) { return unsignedOp(a, b, false); });
    write(dir + "/urem" + s + ".txt", w,
          [](const Integer &a, const Integer &b) { return unsignedOp(a, b, true); });
    write(dir + "/srem" + s + ".txt", w,
          [](const Integer &a, const Integer &b) { return a % b; });
  }
  struct {
    const char *name;
    Float (*op)(const Float &, const Float &);
  } floats[] = {
      {"adderf", [](const Float &a, const Float &b) { return a + b; }},
      {"subf", [](const Float &a, const Float &b) { return a - b; }},
      {"multf", [](const Float &a, const Float &b) { return a * b; }},
      {"divf", [](const Float &a, const Float &b) { return a / b; }},
  };
  for (auto &f : floats) {
    auto op = f.op;
    write(dir + "/" + f.name + ".txt", 32,
          [op](const Integer &a, const Integer &b) {
            return fromFloat(op(toFloat(a), toFloat(b)));
          });
  }
  return 0;
}
