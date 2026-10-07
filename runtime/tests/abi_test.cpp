// Checks every function of the smaug runtime ABI against a C reference.
//
//   abi_test <party> <port>        (run party 1 and party 2 together)
//
// Built once per runtime (abi_test_gmw, abi_test_gc). Inputs come from a
// fixed seed, so both parties know the plaintexts; each party imports its
// XOR share, runs the operation, reveals the result and compares it with the
// reference. Undefined cases (division by zero, INT_MIN / -1, shifts by w or
// more) are kept out of the inputs.

#include "mpc/mpc.h"
#include "mpc/smaug_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace {

using Vals = std::vector<uint64_t>;

const uint32_t Widths[] = {1, 8, 16, 32, 64};
const uint64_t Sizes[] = {0, 1, 7, 33};
int failures = 0, checks = 0;
std::mt19937_64 rng(12345);

uint64_t mask(uint32_t w) { return w == 64 ? ~0ull : ((1ull << w) - 1); }
uint32_t bytes(uint32_t w) { return w == 1 ? 1 : w / 8; }
int64_t sx(uint64_t x, uint32_t w) {
  return w == 64 ? (int64_t)x : (int64_t)(x << (64 - w)) >> (64 - w);
}

Vals randomVals(uint32_t w, uint64_t n) {
  // Mix edge values into random ones.
  const uint64_t edges[] = {0, 1, mask(w), 1ull << (w - 1),
                            mask(w) >> 1, 2, mask(w) - 1};
  Vals v(n);
  for (uint64_t i = 0; i < n; ++i)
    v[i] = (rng() % 4 == 0 ? edges[rng() % 7] : rng()) & mask(w);
  return v;
}

std::vector<uint8_t> pack(const Vals &v, uint32_t w) {
  std::vector<uint8_t> b(v.size() * bytes(w) + 8, 0);
  for (size_t i = 0; i < v.size(); ++i)
    memcpy(&b[i * bytes(w)], &v[i], bytes(w));
  return b;
}

Vals unpack(const std::vector<uint8_t> &b, uint32_t w, uint64_t n) {
  Vals v(n, 0);
  for (uint64_t i = 0; i < n; ++i)
    memcpy(&v[i], &b[i * bytes(w)], bytes(w));
  for (auto &x : v)
    x &= mask(w);
  return v;
}

// A secret buffer holding plaintext v: this party imports its XOR share.
void *secret(const Vals &v, uint32_t w) {
  Vals pad = randomVals(w, v.size()), share(v.size());
  for (size_t i = 0; i < v.size(); ++i)
    share[i] = MPC::party == 1 ? v[i] ^ pad[i] : pad[i];
  void *b = smaug_alloc(w, v.size());
  auto p = pack(share, w);
  smaug_import(w, b, p.data(), v.size());
  return b;
}

Vals reveal(const void *b, uint32_t w, uint64_t n) {
  std::vector<uint8_t> out(n * bytes(w) + 8);
  smaug_reveal(w, out.data(), b, n);
  return unpack(out, w, n);
}

void check(const std::string &what, const Vals &got, const Vals &want) {
  ++checks;
  if (got == want)
    return;
  ++failures;
  for (size_t i = 0; i < want.size(); ++i)
    if (got[i] != want[i]) {
      if (MPC::party == 1)
        printf("FAIL %s [%zu]: got %llx want %llx\n", what.c_str(), i,
               (unsigned long long)got[i], (unsigned long long)want[i]);
      break;
    }
}

// Reference semantics of LLVM integer operations at width w.
uint64_t ref(const std::string &op, uint64_t a, uint64_t b, uint32_t w) {
  int64_t sa = sx(a, w), sb = sx(b, w);
  uint64_t r = 0;
  if (op == "add") r = a + b;
  else if (op == "sub") r = a - b;
  else if (op == "mul") r = a * b;
  else if (op == "sdiv") r = (uint64_t)(sa / sb);
  else if (op == "udiv") r = a / b;
  else if (op == "srem") r = (uint64_t)(sa % sb);
  else if (op == "urem") r = a % b;
  else if (op == "and") r = a & b;
  else if (op == "or") r = a | b;
  else if (op == "xor") r = a ^ b;
  else if (op == "shl") r = a << b;
  else if (op == "lshr") r = a >> b;
  else if (op == "ashr") r = (uint64_t)(sa >> b);
  else if (op == "smin") r = sa < sb ? a : b;
  else if (op == "smax") r = sa > sb ? a : b;
  else if (op == "umin") r = a < b ? a : b;
  else if (op == "umax") r = a > b ? a : b;
  return r & mask(w);
}

bool icmpRef(uint32_t pred, uint64_t a, uint64_t b, uint32_t w) {
  int64_t sa = sx(a, w), sb = sx(b, w);
  switch (pred) {
  case SMAUG_ICMP_EQ: return a == b;
  case SMAUG_ICMP_NE: return a != b;
  case SMAUG_ICMP_UGT: return a > b;
  case SMAUG_ICMP_UGE: return a >= b;
  case SMAUG_ICMP_ULT: return a < b;
  case SMAUG_ICMP_ULE: return a <= b;
  case SMAUG_ICMP_SGT: return sa > sb;
  case SMAUG_ICMP_SGE: return sa >= sb;
  case SMAUG_ICMP_SLT: return sa < sb;
  case SMAUG_ICMP_SLE: return sa <= sb;
  }
  return false;
}

// Makes b valid for op: no division by zero, no INT_MIN / -1, shifts < w.
void sanitize(const std::string &op, Vals &a, Vals &b, uint32_t w) {
  for (size_t i = 0; i < a.size(); ++i) {
    bool div = op == "sdiv" || op == "udiv" || op == "srem" || op == "urem";
    if (div && b[i] == 0)
      b[i] = 1;
    if ((op == "sdiv" || op == "srem") && a[i] == 1ull << (w - 1) &&
        b[i] == mask(w))
      b[i] = 1;
    if (w == 1 && (op == "sdiv" || op == "srem"))
      b[i] = 1; // i1: the only valid divisor is -1 (true).
    if (op == "shl" || op == "lshr" || op == "ashr")
      b[i] %= w;
  }
}

using BinFn = void (*)(uint32_t, void *, const void *, const void *, uint64_t);
using BinSpFn = void (*)(uint32_t, void *, const void *, const void *, uint64_t,
                         uint32_t, uint64_t);

void testBinop(const std::string &op, BinFn f, BinSpFn fsp) {
  for (uint32_t w : Widths)
    for (uint64_t n : Sizes) {
      Vals a = randomVals(w, n), b = randomVals(w, n);
      sanitize(op, a, b, w);
      Vals want(n);
      for (uint64_t i = 0; i < n; ++i)
        want[i] = ref(op, a[i], b[i], w);
      std::string tag = op + " w" + std::to_string(w) + " n" + std::to_string(n);
      void *x = secret(a, w), *y = secret(b, w), *o = smaug_alloc(w, n);
      f(w, o, x, y, n);
      check(tag, reveal(o, w, n), want);
      // In place: out aliases a.
      f(w, x, x, y, n);
      check(tag + " in-place", reveal(x, w, n), want);
      // Public right operand, stride 1 and stride 0.
      smaug_free(x, w, n);
      x = secret(a, w);
      auto pb = pack(b, w);
      fsp(w, o, x, pb.data(), 1, 0, n);
      check(tag + " sp", reveal(o, w, n), want);
      if (n > 0) {
        Vals b0(n, b[0]), want0(n);
        sanitize(op, a, b0, w);
        for (uint64_t i = 0; i < n; ++i)
          want0[i] = ref(op, a[i], b0[i], w);
        auto p0 = pack(b0, w);
        fsp(w, o, x, p0.data(), 0, 0, n);
        check(tag + " sp stride0", reveal(o, w, n), want0);
      }
      // Public left operand.
      Vals pa = randomVals(w, n), sb = b;
      sanitize(op, pa, sb, w);
      Vals wantL(n);
      for (uint64_t i = 0; i < n; ++i)
        wantL[i] = ref(op, pa[i], sb[i], w);
      void *ys = secret(sb, w);
      auto pap = pack(pa, w);
      fsp(w, o, ys, pap.data(), 1, 1, n);
      check(tag + " ps", reveal(o, w, n), wantL);
      for (void *p : {x, y, o, ys})
        smaug_free(p, w, n);
    }
}

void testIcmp() {
  for (uint32_t pred = 32; pred <= 41; ++pred)
    for (uint32_t w : Widths)
      for (uint64_t n : Sizes) {
        Vals a = randomVals(w, n), b = randomVals(w, n);
        for (uint64_t i = 0; i < n; i += 3)
          b[i] = a[i]; // some equal pairs
        Vals want(n);
        for (uint64_t i = 0; i < n; ++i)
          want[i] = icmpRef(pred, a[i], b[i], w);
        std::string tag = "icmp" + std::to_string(pred) + " w" +
                          std::to_string(w) + " n" + std::to_string(n);
        void *x = secret(a, w), *y = secret(b, w), *o = smaug_alloc(1, n);
        smaug_icmp(pred, w, o, x, y, n);
        check(tag, reveal(o, 1, n), want);
        auto pb = pack(b, w);
        smaug_icmp_sp(pred, w, o, x, pb.data(), 1, 0, n);
        check(tag + " sp", reveal(o, 1, n), want);
        Vals wantL(n);
        for (uint64_t i = 0; i < n; ++i)
          wantL[i] = icmpRef(pred, b[i], a[i], w);
        smaug_icmp_sp(pred, w, o, x, pb.data(), 1, 1, n);
        check(tag + " ps", reveal(o, 1, n), wantL);
        for (void *p : {x, y, o})
          smaug_free(p, p == o ? 1 : w, n);
      }
}

void testSelectCastsMemory() {
  for (uint32_t w : Widths)
    for (uint64_t n : Sizes) {
      std::string tag = " w" + std::to_string(w) + " n" + std::to_string(n);
      Vals c = randomVals(1, n), t = randomVals(w, n), f = randomVals(w, n);
      Vals want(n);
      for (uint64_t i = 0; i < n; ++i)
        want[i] = c[i] ? t[i] : f[i];
      void *cs = secret(c, 1), *ts = secret(t, w), *fs = secret(f, w),
           *o = smaug_alloc(w, n);
      smaug_select(w, o, cs, ts, fs, n);
      check("select" + tag, reveal(o, w, n), want);
      auto pc = pack(c, 1);
      smaug_select_pubcond(w, o, pc.data(), 1, ts, fs, n);
      check("select_pubcond" + tag, reveal(o, w, n), want);

      // copy, fill, reverse, export/import round trip.
      smaug_copy(w, o, ts, n);
      check("copy" + tag, reveal(o, w, n), t);
      if (n > 0) {
        smaug_fill(w, o, ts, n);
        check("fill" + tag, reveal(o, w, n), Vals(n, t[0]));
      }
      Vals rev(t.rbegin(), t.rend());
      smaug_reverse(w, o, ts, n);
      check("reverse" + tag, reveal(o, w, n), rev);
      std::vector<uint8_t> sh(n * bytes(w) + 8);
      smaug_export(w, sh.data(), ts, n);
      smaug_import(w, o, sh.data(), n);
      check("export/import" + tag, reveal(o, w, n), t);
      auto pt = pack(t, w);
      smaug_share_public(w, o, pt.data(), 1, n);
      check("share_public" + tag, reveal(o, w, n), t);
      for (void *p : {ts, fs, o})
        smaug_free(p, w, n);
      smaug_free(cs, 1, n);

      // Casts from w to every other width.
      for (uint32_t wd : Widths) {
        if (wd == w)
          continue;
        Vals a = randomVals(w, n), wz(n), ws(n);
        for (uint64_t i = 0; i < n; ++i) {
          wz[i] = a[i] & mask(wd);
          ws[i] = (uint64_t)sx(a[i], w) & mask(wd);
        }
        void *x = secret(a, w), *y = smaug_alloc(wd, n);
        std::string ct = " " + std::to_string(w) + "->" + std::to_string(wd) +
                         " n" + std::to_string(n);
        if (wd > w) {
          smaug_zext(wd, w, y, x, n);
          check("zext" + ct, reveal(y, wd, n), wz);
          smaug_sext(wd, w, y, x, n);
          check("sext" + ct, reveal(y, wd, n), ws);
        } else {
          smaug_trunc(wd, w, y, x, n);
          check("trunc" + ct, reveal(y, wd, n), wz);
        }
        smaug_free(x, w, n);
        smaug_free(y, wd, n);
      }
    }
}

void testReduce() {
  const char *names[] = {"", "add", "mul", "and", "or", "xor",
                         "smax", "smin", "umax", "umin"};
  for (uint32_t op = SMAUG_RED_ADD; op <= SMAUG_RED_UMIN; ++op)
    for (uint32_t w : Widths)
      for (uint64_t n : Sizes) {
        Vals a = randomVals(w, n);
        std::string name = names[op];
        uint64_t acc = 0;
        if (op == SMAUG_RED_MUL) acc = 1;
        if (op == SMAUG_RED_AND || op == SMAUG_RED_UMIN) acc = mask(w);
        if (op == SMAUG_RED_SMAX) acc = 1ull << (w - 1);
        if (op == SMAUG_RED_SMIN) acc = mask(w) >> 1;
        for (uint64_t x : a)
          acc = ref(name, acc, x, w);
        void *s = secret(a, w), *o = smaug_alloc(w, 1);
        smaug_reduce(op, w, o, s, n);
        check("reduce " + name + " w" + std::to_string(w) + " n" +
                  std::to_string(n),
              reveal(o, w, 1), Vals{acc});
        smaug_free(s, w, n);
        smaug_free(o, w, 1);
      }
}

void testOblivious() {
  for (uint32_t w : Widths)
    for (uint32_t iw : {8u, 32u, 64u}) {
      uint64_t len = 9;
      Vals buf = randomVals(w, len);
      for (uint64_t idx : {0ull, 4ull, 8ull, 9ull, 200ull}) {
        std::string tag = " w" + std::to_string(w) + " iw" +
                          std::to_string(iw) + " idx" + std::to_string(idx);
        void *b = secret(buf, w), *i = secret(Vals{idx & mask(iw)}, iw),
             *o = smaug_alloc(w, 1);
        smaug_oload(w, o, b, len, i, iw);
        check("oload" + tag, reveal(o, w, 1), Vals{idx < len ? buf[idx] : 0});
        Vals v = randomVals(w, 1), after = buf;
        if (idx < len)
          after[idx] = v[0];
        void *vs = secret(v, w);
        smaug_ostore(w, b, len, i, iw, vs);
        check("ostore" + tag, reveal(b, w, len), after);
        smaug_free(b, w, len);
        smaug_free(i, iw, 1);
        smaug_free(o, w, 1);
        smaug_free(vs, w, 1);
      }
    }
}

uint32_t fbits(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}
float ffloat(uint64_t u) {
  uint32_t x = (uint32_t)u;
  float f;
  memcpy(&f, &x, 4);
  return f;
}

void checkFloat(const std::string &what, const Vals &got,
                const std::vector<float> &want) {
  ++checks;
  for (size_t i = 0; i < want.size(); ++i) {
    float g = ffloat(got[i]), e = want[i];
    if (std::fabs(g - e) > 1e-4f * std::fmax(1.0f, std::fabs(e))) {
      ++failures;
      if (MPC::party == 1)
        printf("FAIL %s [%zu]: got %g want %g\n", what.c_str(), i, g, e);
      return;
    }
  }
}

void testFloat() {
  std::uniform_real_distribution<float> d(-100.0f, 100.0f);
  for (uint64_t n : Sizes) {
    std::vector<float> a(n), b(n);
    Vals av(n), bv(n);
    for (uint64_t i = 0; i < n; ++i) {
      a[i] = d(rng);
      b[i] = d(rng);
      if (std::fabs(b[i]) < 0.5f)
        b[i] = 1.5f;
      av[i] = fbits(a[i]);
      bv[i] = fbits(b[i]);
    }
    void *x = secret(av, 32), *y = secret(bv, 32), *o = smaug_alloc(32, n);
    std::string tag = " n" + std::to_string(n);
    struct {
      const char *name;
      BinFn f;
      std::function<float(float, float)> r;
    } ops[] = {{"fadd", smaug_fadd, [](float p, float q) { return p + q; }},
               {"fsub", smaug_fsub, [](float p, float q) { return p - q; }},
               {"fmul", smaug_fmul, [](float p, float q) { return p * q; }},
               {"fdiv", smaug_fdiv, [](float p, float q) { return p / q; }}};
    for (auto &op : ops) {
      std::vector<float> want(n);
      for (uint64_t i = 0; i < n; ++i)
        want[i] = op.r(a[i], b[i]);
      op.f(32, o, x, y, n);
      checkFloat(std::string(op.name) + tag, reveal(o, 32, n), want);
    }
    for (uint32_t pred = SMAUG_FCMP_OEQ; pred <= SMAUG_FCMP_ONE; ++pred) {
      Vals want(n);
      for (uint64_t i = 0; i < n; ++i) {
        float p = a[i], q = i % 3 ? b[i] : a[i];
        want[i] = pred == SMAUG_FCMP_OEQ   ? p == q
                  : pred == SMAUG_FCMP_OGT ? p > q
                  : pred == SMAUG_FCMP_OGE ? p >= q
                  : pred == SMAUG_FCMP_OLT ? p < q
                  : pred == SMAUG_FCMP_OLE ? p <= q
                                           : p != q;
      }
      Vals qv(n);
      for (uint64_t i = 0; i < n; ++i)
        qv[i] = i % 3 ? bv[i] : av[i];
      void *q = secret(qv, 32), *c = smaug_alloc(1, n);
      smaug_fcmp(pred, 32, c, x, q, n);
      check("fcmp" + std::to_string(pred) + tag, reveal(c, 1, n), want);
      smaug_free(q, 32, n);
      smaug_free(c, 1, n);
    }
    smaug_free(x, 32, n);
    smaug_free(y, 32, n);
    smaug_free(o, 32, n);
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <party> <port>\n", argv[0]);
    return 2;
  }
  MPC::setup(atoi(argv[1]), atoi(argv[2]));
  struct {
    const char *name;
    BinFn f;
    BinSpFn sp;
  } binops[] = {{"add", smaug_add, smaug_add_sp},
                {"sub", smaug_sub, smaug_sub_sp},
                {"mul", smaug_mul, smaug_mul_sp},
                {"sdiv", smaug_sdiv, smaug_sdiv_sp},
                {"udiv", smaug_udiv, smaug_udiv_sp},
                {"srem", smaug_srem, smaug_srem_sp},
                {"urem", smaug_urem, smaug_urem_sp},
                {"and", smaug_and, smaug_and_sp},
                {"or", smaug_or, smaug_or_sp},
                {"xor", smaug_xor, smaug_xor_sp},
                {"shl", smaug_shl, smaug_shl_sp},
                {"lshr", smaug_lshr, smaug_lshr_sp},
                {"ashr", smaug_ashr, smaug_ashr_sp},
                {"smin", smaug_smin, smaug_smin_sp},
                {"smax", smaug_smax, smaug_smax_sp},
                {"umin", smaug_umin, smaug_umin_sp},
                {"umax", smaug_umax, smaug_umax_sp}};
  const char *only = argc > 3 ? argv[3] : nullptr;
  auto want = [&](const char *group) {
    return !only || !strcmp(only, group);
  };
  for (auto &b : binops)
    if (want(b.name) || want("binops"))
      testBinop(b.name, b.f, b.sp);
  if (want("icmp"))
    testIcmp();
  if (want("misc"))
    testSelectCastsMemory();
  if (want("reduce"))
    testReduce();
  if (want("oblivious"))
    testOblivious();
  if (want("float"))
    testFloat();
  if (MPC::party == 1)
    printf("%d checks, %d failures\n", checks, failures);
  MPC::finish();
  return failures ? 1 : 0;
}
