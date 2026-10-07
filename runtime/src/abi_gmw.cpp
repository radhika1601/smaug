// The smaug_* runtime ABI (mpc/smaug_abi.def) for the GMW backend. A secret
// buffer holds this party's XOR shares in the plain layout: one byte per
// element for width 1, w / 8 bytes otherwise. Floats are 32-bit patterns.
//
// Work is done on 64-bit share values. Linear operations (xor, constants,
// public shifts, casts) are local; AND gates and the circuits in
// MPC_CIRCUIT_DIR run through mpc.cpp on bit-sliced copies. The emp-aby
// headers define non-inline functions, so only mpc.cpp includes them.

#include "emp-tool/io/net_io_channel.h"
#include "mpc/smaug_abi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef MPC_CIRCUIT_DIR
#define MPC_CIRCUIT_DIR "/usr/local/include/mpc/circuits/"
#endif

using namespace emp;

namespace MPC {
extern int party;
extern std::vector<NetIO *> ios;
// Defined in mpc.cpp.
void abiAndGate(bool *out, bool *a, bool *b, size_t n);
void abiRunCircuit(const char *path, bool *out, bool *in, unsigned num);
} // namespace MPC

namespace {

using Vals = std::vector<uint64_t>;

[[noreturn]] void fail(const char *what, uint32_t w) {
  fprintf(stderr, "smaug runtime (gmw): %s not supported for width %u\n", what,
          w);
  abort();
}

void checkWidth(uint32_t w) {
  if (w != 1 && w != 8 && w != 16 && w != 32 && w != 64)
    fail("element width", w);
}

uint32_t storageBytes(uint32_t w) { return w == 1 ? 1 : w / 8; }
uint64_t mask(uint32_t w) { return w == 64 ? ~0ull : ((1ull << w) - 1); }
bool isAlice() { return MPC::party == 1; }

// A public constant as a share: party 1 holds it, party 2 holds 0.
uint64_t aliceOnly(uint64_t v) { return isAlice() ? v : 0; }

uint64_t readRaw(const void *b, uint32_t w, uint64_t k) {
  const uint8_t *p = (const uint8_t *)b + k * storageBytes(w);
  switch (w) {
  case 1:
    return p[0] & 1;
  case 8:
    return p[0];
  case 16:
    return *(const uint16_t *)p;
  case 32:
    return *(const uint32_t *)p;
  default:
    return *(const uint64_t *)p;
  }
}

void writeRaw(void *b, uint32_t w, uint64_t k, uint64_t v) {
  uint8_t *p = (uint8_t *)b + k * storageBytes(w);
  switch (w) {
  case 1:
    p[0] = v & 1;
    break;
  case 8:
    p[0] = (uint8_t)v;
    break;
  case 16:
    *(uint16_t *)p = (uint16_t)v;
    break;
  case 32:
    *(uint32_t *)p = (uint32_t)v;
    break;
  default:
    *(uint64_t *)p = v;
  }
}

Vals load(const void *b, uint32_t w, uint64_t n) {
  Vals v(n);
  for (uint64_t i = 0; i < n; ++i)
    v[i] = readRaw(b, w, i);
  return v;
}

void store(void *b, uint32_t w, const Vals &v) {
  for (uint64_t i = 0; i < v.size(); ++i)
    writeRaw(b, w, i, v[i]);
}

// Shares of a public operand: party 1 holds the values.
Vals publicShares(const void *pub, uint32_t w, uint64_t pstride, uint64_t n) {
  Vals v(n);
  for (uint64_t i = 0; i < n; ++i)
    v[i] = aliceOnly(readRaw(pub, w, i * pstride));
  return v;
}

Vals xorv(const Vals &a, const Vals &b) {
  Vals r(a.size());
  for (size_t i = 0; i < a.size(); ++i)
    r[i] = a[i] ^ b[i];
  return r;
}

Vals xorConst(const Vals &a, uint64_t c) {
  Vals r(a.size());
  for (size_t i = 0; i < a.size(); ++i)
    r[i] = a[i] ^ aliceOnly(c);
  return r;
}

std::unique_ptr<bool[]> slice(const Vals &v, uint32_t w) {
  std::unique_ptr<bool[]> b(new bool[v.size() * w + 1]);
  for (size_t i = 0; i < v.size(); ++i)
    for (uint32_t j = 0; j < w; ++j)
      b[i * w + j] = (v[i] >> j) & 1;
  return b;
}

Vals unslice(const bool *b, uint64_t n, uint32_t w) {
  Vals v(n, 0);
  for (uint64_t i = 0; i < n; ++i)
    for (uint32_t j = 0; j < w; ++j)
      v[i] |= (uint64_t)b[i * w + j] << j;
  return v;
}

// Bitwise AND of two share vectors of width w: one AND layer.
Vals andv(const Vals &a, const Vals &b, uint32_t w) {
  uint64_t m = a.size() * w;
  if (m == 0)
    return Vals(a.size(), 0);
  auto x = slice(a, w), y = slice(b, w);
  std::unique_ptr<bool[]> o(new bool[m]);
  MPC::abiAndGate(o.get(), x.get(), y.get(), m);
  return unslice(o.get(), a.size(), w);
}

// A share of a bit broadcast to all w bits: copying a bit is linear.
Vals broadcast(const Vals &bits, uint32_t w) {
  Vals r(bits.size());
  for (size_t i = 0; i < bits.size(); ++i)
    r[i] = (bits[i] & 1) ? mask(w) : 0;
  return r;
}

// cond ? t : f, with cond a vector of bit shares.
Vals mux(const Vals &cond, const Vals &t, const Vals &f, uint32_t w) {
  return xorv(f, andv(broadcast(cond, w), xorv(t, f), w));
}

bool fileExists(const std::string &path) {
  FILE *f = fopen(path.c_str(), "r");
  if (f)
    fclose(f);
  return f != nullptr;
}

std::string circuitPath(const std::string &name) {
  return std::string(MPC_CIRCUIT_DIR) + name + ".txt";
}

bool hasCircuit(const std::string &name) {
  static std::map<std::string, bool> known;
  auto it = known.find(name);
  if (it == known.end())
    it = known.emplace(name, fileExists(circuitPath(name))).first;
  return it->second;
}

// Runs a two-input circuit over n instances. Inputs are packed as all of a,
// then all of b, LSB first; each instance has wo output bits.
Vals runCircuit(const std::string &name, uint32_t wi, uint32_t wo,
                const Vals &a, const Vals &b) {
  uint64_t n = a.size();
  if (n == 0)
    return Vals();
  std::unique_ptr<bool[]> in(new bool[2 * n * wi]);
  auto x = slice(a, wi), y = slice(b, wi);
  memcpy(in.get(), x.get(), n * wi);
  memcpy(in.get() + n * wi, y.get(), n * wi);
  if (!hasCircuit(name)) {
    fprintf(stderr, "smaug runtime (gmw): missing circuit %s\n",
            circuitPath(name).c_str());
    abort();
  }
  std::unique_ptr<bool[]> out(new bool[n * wo]);
  MPC::abiRunCircuit(circuitPath(name).c_str(), out.get(), in.get(), n);
  return unslice(out.get(), n, wo);
}

std::string wname(const char *base, uint32_t w) {
  return std::string(base) + std::to_string(w);
}

Vals signFlip(const Vals &a, uint32_t w) { return xorConst(a, 1ull << (w - 1)); }

// Comparison results are bit shares.
Vals cmpGt(const Vals &a, const Vals &b, uint32_t w) {
  return runCircuit(wname("gt", w), w, 1, a, b);
}
Vals cmpGe(const Vals &a, const Vals &b, uint32_t w) {
  return runCircuit(wname("ge", w), w, 1, a, b);
}
Vals cmpEq(const Vals &a, const Vals &b, uint32_t w) {
  return runCircuit(wname("icmpeq", w), w, 1, a, b);
}

Vals notBits(const Vals &a) { return xorConst(a, 1); }

Vals icmpBits(uint32_t pred, const Vals &a, const Vals &b) {
  // i1 values: true is 1 unsigned and -1 signed.
  switch (pred) {
  case SMAUG_ICMP_EQ:
    return notBits(xorv(a, b));
  case SMAUG_ICMP_NE:
    return xorv(a, b);
  case SMAUG_ICMP_UGT:
  case SMAUG_ICMP_SLT:
    return andv(a, notBits(b), 1);
  case SMAUG_ICMP_ULT:
  case SMAUG_ICMP_SGT:
    return andv(notBits(a), b, 1);
  case SMAUG_ICMP_UGE:
  case SMAUG_ICMP_SLE:
    return notBits(andv(notBits(a), b, 1));
  case SMAUG_ICMP_ULE:
  case SMAUG_ICMP_SGE:
    return notBits(andv(a, notBits(b), 1));
  }
  fail("icmp predicate", pred);
}

Vals icmpv(uint32_t pred, const Vals &a, const Vals &b, uint32_t w) {
  if (w == 1)
    return icmpBits(pred, a, b);
  switch (pred) {
  case SMAUG_ICMP_EQ:
    return cmpEq(a, b, w);
  case SMAUG_ICMP_NE:
    return notBits(cmpEq(a, b, w));
  case SMAUG_ICMP_SGT:
    return cmpGt(a, b, w);
  case SMAUG_ICMP_SGE:
    return cmpGe(a, b, w);
  case SMAUG_ICMP_SLT:
    return cmpGt(b, a, w);
  case SMAUG_ICMP_SLE:
    return cmpGe(b, a, w);
  case SMAUG_ICMP_UGT:
    return cmpGt(signFlip(a, w), signFlip(b, w), w);
  case SMAUG_ICMP_UGE:
    return cmpGe(signFlip(a, w), signFlip(b, w), w);
  case SMAUG_ICMP_ULT:
    return cmpGt(signFlip(b, w), signFlip(a, w), w);
  case SMAUG_ICMP_ULE:
    return cmpGe(signFlip(b, w), signFlip(a, w), w);
  }
  fail("icmp predicate", pred);
}

enum class Op { Add, Sub, Mul, SDiv, UDiv, SRem, URem, And, Or, Xor, Shl, LShr, AShr, SMin, SMax, UMin, UMax };

uint64_t signExtend(uint64_t x, uint32_t w) {
  if (w == 64)
    return x;
  return (uint64_t)((int64_t)(x << (64 - w)) >> (64 - w));
}

// Shifts of one share by a public amount; shifting is linear.
uint64_t shiftShare(Op op, uint64_t x, uint64_t k, uint32_t w) {
  if (k >= w)
    return op == Op::AShr ? (signExtend(x, w) >> 63 ? mask(w) : 0) : 0;
  switch (op) {
  case Op::Shl:
    return (x << k) & mask(w);
  case Op::LShr:
    return (x & mask(w)) >> k;
  default:
    return (uint64_t)((int64_t)signExtend(x, w) >> k) & mask(w);
  }
}

Vals shiftPub(Op op, const Vals &a, const Vals &amounts, uint32_t w) {
  Vals r(a.size());
  for (size_t i = 0; i < a.size(); ++i)
    r[i] = shiftShare(op, a[i], amounts[i], w);
  return r;
}

// Shift by a secret amount: one mux layer per bit of the amount.
Vals shiftSecret(Op op, Vals x, const Vals &amount, uint32_t w) {
  for (uint32_t s = 0; (1u << s) < w; ++s) {
    Vals bit(x.size()), shifted(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
      bit[i] = (amount[i] >> s) & 1;
      shifted[i] = shiftShare(op, x[i], 1ull << s, w);
    }
    x = mux(bit, shifted, x, w);
  }
  return x;
}

Vals divRem(Op op, const Vals &a, const Vals &b, uint32_t w);

Vals binv(Op op, const Vals &a, const Vals &b, uint32_t w) {
  if (w == 1) {
    // i1: add and sub are xor, mul is and, division returns the dividend,
    // remainders are 0, shifts are by 0; signed order has true below false.
    switch (op) {
    case Op::Add:
    case Op::Sub:
    case Op::Xor:
      return xorv(a, b);
    case Op::Mul:
    case Op::And:
    case Op::SMax:
    case Op::UMin:
      return andv(a, b, 1);
    case Op::Or:
    case Op::SMin:
    case Op::UMax:
      return xorv(xorv(a, b), andv(a, b, 1));
    case Op::SDiv:
    case Op::UDiv:
    case Op::Shl:
    case Op::LShr:
    case Op::AShr:
      return a;
    case Op::SRem:
    case Op::URem:
      return Vals(a.size(), 0);
    }
  }
  switch (op) {
  case Op::Add:
    return runCircuit(wname("adder", w), w, w, a, b);
  case Op::Sub:
    return runCircuit(wname("sub", w), w, w, a, b);
  case Op::Mul:
    return runCircuit(wname("mult", w), w, w, a, b);
  case Op::SDiv:
  case Op::UDiv:
  case Op::SRem:
  case Op::URem:
    return divRem(op, a, b, w);
  case Op::And:
    return andv(a, b, w);
  case Op::Or:
    return xorv(xorv(a, b), andv(a, b, w));
  case Op::Xor:
    return xorv(a, b);
  case Op::Shl:
  case Op::LShr:
  case Op::AShr:
    return shiftSecret(op, a, b, w);
  case Op::SMin:
    return mux(cmpGt(a, b, w), b, a, w);
  case Op::SMax:
    return mux(cmpGt(a, b, w), a, b, w);
  case Op::UMin:
    return mux(cmpGt(signFlip(a, w), signFlip(b, w), w), b, a, w);
  case Op::UMax:
    return mux(cmpGt(signFlip(a, w), signFlip(b, w), w), a, b, w);
  }
  abort();
}

// Division and remainder. A dedicated circuit (udiv{w}, urem{w}, srem{w})
// is used when installed; otherwise unsigned division widens to 2w bits and
// uses the signed circuit, and a remainder is a - (a / b) * b.
Vals divRem(Op op, const Vals &a, const Vals &b, uint32_t w) {
  const char *base = op == Op::SDiv   ? "div"
                     : op == Op::UDiv ? "udiv"
                     : op == Op::SRem ? "srem"
                                      : "urem";
  if (hasCircuit(wname(base, w)))
    return runCircuit(wname(base, w), w, w, a, b);
  if (op == Op::UDiv) {
    if (w > 32)
      fail("udiv without a udiv64 circuit", w);
    // Zero-extended shares are shares of the zero-extended value.
    Vals q = runCircuit(wname("div", 2 * w), 2 * w, 2 * w, a, b);
    for (auto &x : q)
      x &= mask(w);
    return q;
  }
  Op div = op == Op::SRem ? Op::SDiv : Op::UDiv;
  Vals q = divRem(div, a, b, w);
  return binv(Op::Sub, a, binv(Op::Mul, q, b, w), w);
}

Vals binvPub(Op op, const Vals &a, const void *pub, uint64_t pstride,
             bool pubIsLhs, uint32_t w) {
  uint64_t n = a.size();
  Vals raw(n);
  for (uint64_t i = 0; i < n; ++i)
    raw[i] = readRaw(pub, w, i * pstride);
  bool shift = op == Op::Shl || op == Op::LShr || op == Op::AShr;
  if (!pubIsLhs && (shift || op == Op::And || op == Op::Xor || op == Op::Or)) {
    if (shift)
      return w == 1 ? a : shiftPub(op, a, raw, w);
    if (op == Op::And) {
      // Both parties mask their shares with the public value.
      Vals r(n);
      for (uint64_t i = 0; i < n; ++i)
        r[i] = a[i] & raw[i];
      return r;
    }
    if (op == Op::Xor)
      return xorv(a, publicShares(pub, w, pstride, n));
    // a | p = (a & ~p) ^ p.
    Vals r(n);
    for (uint64_t i = 0; i < n; ++i)
      r[i] = (a[i] & ~raw[i] & mask(w)) ^ aliceOnly(raw[i]);
    return r;
  }
  Vals p = publicShares(pub, w, pstride, n);
  return pubIsLhs ? binv(op, p, a, w) : binv(op, a, p, w);
}

Vals reduceTree(Op op, Vals v, uint32_t w) {
  while (v.size() > 1) {
    size_t half = v.size() / 2;
    Vals lo(v.begin(), v.begin() + half), hi(v.begin() + half, v.begin() + 2 * half);
    Vals r = binv(op, lo, hi, w);
    if (v.size() % 2)
      r.push_back(v.back());
    v = std::move(r);
  }
  return v;
}

// One equality bit share per position 0..len-1 against a secret index.
Vals positionHits(const void *idx, uint32_t idx_w, uint64_t len) {
  uint64_t share = readRaw(idx, idx_w, 0);
  if (idx_w == 1) {
    Vals hits(len, 0);
    for (uint64_t i = 0; i < len && i < 2; ++i)
      hits[i] = i ? share : aliceOnly(1) ^ share;
    return hits;
  }
  Vals a(len, share), positions(len);
  for (uint64_t i = 0; i < len; ++i)
    positions[i] = aliceOnly(i & mask(idx_w));
  return cmpEq(a, positions, idx_w);
}

enum class FOp { Add, Sub, Mul, Div };

Vals floatv(FOp op, const Vals &a, const Vals &b) {
  const char *name = op == FOp::Add   ? "adderf"
                     : op == FOp::Sub ? "subf"
                     : op == FOp::Mul ? "multf"
                                      : "divf";
  return runCircuit(name, 32, 32, a, b);
}

Vals fcmpv(uint32_t pred, const Vals &a, const Vals &b) {
  switch (pred) {
  case SMAUG_FCMP_OEQ:
    return runCircuit("icmpeqf", 32, 1, a, b);
  case SMAUG_FCMP_ONE:
    return notBits(runCircuit("icmpeqf", 32, 1, a, b));
  case SMAUG_FCMP_OGT:
    return runCircuit("gtf", 32, 1, a, b);
  case SMAUG_FCMP_OGE:
    return runCircuit("gef", 32, 1, a, b);
  case SMAUG_FCMP_OLT:
    return runCircuit("gtf", 32, 1, b, a);
  case SMAUG_FCMP_OLE:
    return runCircuit("gef", 32, 1, b, a);
  }
  fail("fcmp predicate", pred);
}

} // namespace

extern "C" {

void *smaug_alloc(uint32_t w, uint64_t n) {
  checkWidth(w);
  return calloc(n ? n : 1, storageBytes(w));
}

void smaug_free(void *b, uint32_t, uint64_t) { free(b); }

void *smaug_elem(void *b, uint32_t w, uint64_t i) {
  return (uint8_t *)b + i * storageBytes(w);
}

int32_t smaug_party(void) { return MPC::party; }

void smaug_copy(uint32_t w, void *dst, const void *src, uint64_t n) {
  memmove(dst, src, n * storageBytes(w));
}

void smaug_fill(uint32_t w, void *dst, const void *src1, uint64_t n) {
  uint64_t v = readRaw(src1, w, 0);
  for (uint64_t i = 0; i < n; ++i)
    writeRaw(dst, w, i, v);
}

void smaug_reverse(uint32_t w, void *dst, const void *src, uint64_t n) {
  Vals v = load(src, w, n);
  for (uint64_t i = 0; i < n; ++i)
    writeRaw(dst, w, i, v[n - 1 - i]);
}

void smaug_import(uint32_t w, void *dst, const void *share, uint64_t n) {
  memmove(dst, share, n * storageBytes(w));
}

void smaug_export(uint32_t w, void *share, const void *src, uint64_t n) {
  memmove(share, src, n * storageBytes(w));
}

void smaug_share_public(uint32_t w, void *dst, const void *pub,
                        uint64_t pstride, uint64_t n) {
  checkWidth(w);
  store(dst, w, publicShares(pub, w, pstride, n));
}

void smaug_reveal(uint32_t w, void *pub, const void *src, uint64_t n) {
  checkWidth(w);
  Vals mine = load(src, w, n), theirs(n);
  NetIO *io = MPC::ios[0];
  if (isAlice()) {
    io->send_data(mine.data(), n * 8);
    io->flush();
    io->recv_data(theirs.data(), n * 8);
  } else {
    io->recv_data(theirs.data(), n * 8);
    io->send_data(mine.data(), n * 8);
    io->flush();
  }
  store(pub, w, xorv(mine, theirs));
}

#define BINOP(name, op)                                                        \
  void smaug_##name(uint32_t w, void *out, const void *a, const void *b,       \
                    uint64_t n) {                                              \
    checkWidth(w);                                                             \
    store(out, w, binv(op, load(a, w, n), load(b, w, n), w));                  \
  }                                                                            \
  void smaug_##name##_sp(uint32_t w, void *out, const void *a,                 \
                         const void *pub, uint64_t pstride,                    \
                         uint32_t pub_is_lhs, uint64_t n) {                    \
    checkWidth(w);                                                             \
    store(out, w, binvPub(op, load(a, w, n), pub, pstride, pub_is_lhs, w));    \
  }
BINOP(add, Op::Add)
BINOP(sub, Op::Sub)
BINOP(mul, Op::Mul)
BINOP(sdiv, Op::SDiv)
BINOP(udiv, Op::UDiv)
BINOP(srem, Op::SRem)
BINOP(urem, Op::URem)
BINOP(shl, Op::Shl)
BINOP(lshr, Op::LShr)
BINOP(ashr, Op::AShr)
BINOP(smin, Op::SMin)
BINOP(smax, Op::SMax)
BINOP(umin, Op::UMin)
BINOP(umax, Op::UMax)
#undef BINOP

// and, or and xor are operator tokens in C++, so these are written out.
void smaug_and(uint32_t w, void *out, const void *a, const void *b, uint64_t n) {
  checkWidth(w);
  store(out, w, binv(Op::And, load(a, w, n), load(b, w, n), w));
}
void smaug_and_sp(uint32_t w, void *out, const void *a, const void *pub,
                  uint64_t pstride, uint32_t pub_is_lhs, uint64_t n) {
  checkWidth(w);
  store(out, w, binvPub(Op::And, load(a, w, n), pub, pstride, pub_is_lhs, w));
}
void smaug_or(uint32_t w, void *out, const void *a, const void *b, uint64_t n) {
  checkWidth(w);
  store(out, w, binv(Op::Or, load(a, w, n), load(b, w, n), w));
}
void smaug_or_sp(uint32_t w, void *out, const void *a, const void *pub,
                 uint64_t pstride, uint32_t pub_is_lhs, uint64_t n) {
  checkWidth(w);
  store(out, w, binvPub(Op::Or, load(a, w, n), pub, pstride, pub_is_lhs, w));
}
void smaug_xor(uint32_t w, void *out, const void *a, const void *b, uint64_t n) {
  checkWidth(w);
  store(out, w, binv(Op::Xor, load(a, w, n), load(b, w, n), w));
}
void smaug_xor_sp(uint32_t w, void *out, const void *a, const void *pub,
                  uint64_t pstride, uint32_t pub_is_lhs, uint64_t n) {
  checkWidth(w);
  store(out, w, binvPub(Op::Xor, load(a, w, n), pub, pstride, pub_is_lhs, w));
}

void smaug_icmp(uint32_t pred, uint32_t w, void *out, const void *a,
                const void *b, uint64_t n) {
  checkWidth(w);
  store(out, 1, icmpv(pred, load(a, w, n), load(b, w, n), w));
}

void smaug_icmp_sp(uint32_t pred, uint32_t w, void *out, const void *a,
                   const void *pub, uint64_t pstride, uint32_t pub_is_lhs,
                   uint64_t n) {
  checkWidth(w);
  Vals x = load(a, w, n), p = publicShares(pub, w, pstride, n);
  store(out, 1, pub_is_lhs ? icmpv(pred, p, x, w) : icmpv(pred, x, p, w));
}

void smaug_select(uint32_t w, void *out, const void *cond, const void *t,
                  const void *f, uint64_t n) {
  checkWidth(w);
  store(out, w, mux(load(cond, 1, n), load(t, w, n), load(f, w, n), w));
}

void smaug_select_pubcond(uint32_t w, void *out, const void *pcond,
                          uint64_t pstride, const void *t, const void *f,
                          uint64_t n) {
  checkWidth(w);
  Vals tv = load(t, w, n), fv = load(f, w, n), r(n);
  for (uint64_t i = 0; i < n; ++i)
    r[i] = readRaw(pcond, 1, i * pstride) ? tv[i] : fv[i];
  store(out, w, r);
}

// Width changes are linear on XOR shares.
void smaug_zext(uint32_t wd, uint32_t ws, void *out, const void *a, uint64_t n) {
  checkWidth(wd);
  checkWidth(ws);
  Vals v = load(a, ws, n);
  for (auto &x : v)
    x &= mask(ws);
  store(out, wd, v);
}

void smaug_sext(uint32_t wd, uint32_t ws, void *out, const void *a, uint64_t n) {
  checkWidth(wd);
  checkWidth(ws);
  Vals v = load(a, ws, n);
  for (auto &x : v)
    x = signExtend(x, ws) & mask(wd);
  store(out, wd, v);
}

void smaug_trunc(uint32_t wd, uint32_t ws, void *out, const void *a, uint64_t n) {
  checkWidth(wd);
  checkWidth(ws);
  Vals v = load(a, ws, n);
  for (auto &x : v)
    x &= mask(wd);
  store(out, wd, v);
}

void smaug_reduce(uint32_t op, uint32_t w, void *out1, const void *a,
                  uint64_t n) {
  checkWidth(w);
  Op o;
  switch (op) {
  case SMAUG_RED_ADD: o = Op::Add; break;
  case SMAUG_RED_MUL: o = Op::Mul; break;
  case SMAUG_RED_AND: o = Op::And; break;
  case SMAUG_RED_OR: o = Op::Or; break;
  case SMAUG_RED_XOR: o = Op::Xor; break;
  case SMAUG_RED_SMAX: o = Op::SMax; break;
  case SMAUG_RED_SMIN: o = Op::SMin; break;
  case SMAUG_RED_UMAX: o = Op::UMax; break;
  case SMAUG_RED_UMIN: o = Op::UMin; break;
  default: fail("reduction", op);
  }
  if (n == 0) {
    // Identity of the operation.
    uint64_t id = 0;
    if (o == Op::Mul) id = 1;
    if (o == Op::And || o == Op::UMin) id = mask(w);
    if (o == Op::SMax) id = 1ull << (w - 1);
    if (o == Op::SMin) id = mask(w) >> 1;
    writeRaw(out1, w, 0, aliceOnly(id));
    return;
  }
  Vals v = load(a, w, n);
  if (o == Op::Xor) {
    uint64_t r = 0;
    for (uint64_t x : v)
      r ^= x;
    writeRaw(out1, w, 0, r);
    return;
  }
  writeRaw(out1, w, 0, reduceTree(o, v, w)[0]);
}

void smaug_oload(uint32_t w, void *out1, const void *buf, uint64_t len,
                 const void *idx, uint32_t idx_w) {
  checkWidth(w);
  checkWidth(idx_w);
  Vals picked = andv(broadcast(positionHits(idx, idx_w, len), w),
                     load(buf, w, len), w);
  uint64_t r = 0;
  for (uint64_t x : picked)
    r ^= x;
  writeRaw(out1, w, 0, r);
}

void smaug_ostore(uint32_t w, void *buf, uint64_t len, const void *idx,
                  uint32_t idx_w, const void *val1) {
  checkWidth(w);
  checkWidth(idx_w);
  Vals old = load(buf, w, len);
  Vals val(len, readRaw(val1, w, 0));
  store(buf, w, mux(positionHits(idx, idx_w, len), val, old, w));
}

#define FBINOP(name, op)                                                       \
  void smaug_##name(uint32_t w, void *out, const void *a, const void *b,       \
                    uint64_t n) {                                              \
    if (w != 32)                                                               \
      fail("float operation", w);                                              \
    store(out, 32, floatv(op, load(a, 32, n), load(b, 32, n)));                \
  }                                                                            \
  void smaug_##name##_sp(uint32_t w, void *out, const void *a,                 \
                         const void *pub, uint64_t pstride,                    \
                         uint32_t pub_is_lhs, uint64_t n) {                    \
    if (w != 32)                                                               \
      fail("float operation", w);                                              \
    Vals x = load(a, 32, n), p = publicShares(pub, 32, pstride, n);            \
    store(out, 32, pub_is_lhs ? floatv(op, p, x) : floatv(op, x, p));          \
  }
FBINOP(fadd, FOp::Add)
FBINOP(fsub, FOp::Sub)
FBINOP(fmul, FOp::Mul)
FBINOP(fdiv, FOp::Div)
#undef FBINOP

void smaug_fcmp(uint32_t pred, uint32_t w, void *out, const void *a,
                const void *b, uint64_t n) {
  if (w != 32)
    fail("float comparison", w);
  store(out, 1, fcmpv(pred, load(a, 32, n), load(b, 32, n)));
}

void smaug_fcmp_sp(uint32_t pred, uint32_t w, void *out, const void *a,
                   const void *pub, uint64_t pstride, uint32_t pub_is_lhs,
                   uint64_t n) {
  if (w != 32)
    fail("float comparison", w);
  Vals x = load(a, 32, n), p = publicShares(pub, 32, pstride, n);
  store(out, 1, pub_is_lhs ? fcmpv(pred, p, x) : fcmpv(pred, x, p));
}

} // extern "C"
