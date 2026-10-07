// The smaug_* runtime ABI (mpc/smaug_abi.def) for the garbled circuit
// backend. A secret buffer of width 1 is an array of emp::Bit; any other
// width is an array of emp::Integer of that width. Floats are stored as their
// 32-bit patterns in Integers and converted to emp::Float only to compute.

#include "emp-sh2pc/emp-sh2pc.h"
#include "mpc/smaug_abi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace emp;

namespace MPC {
extern int party;
}

namespace {

[[noreturn]] void fail(const char *what, uint32_t w) {
  fprintf(stderr, "smaug runtime (gc): %s not supported for width %u\n", what,
          w);
  abort();
}

void checkWidth(uint32_t w) {
  if (w != 1 && w != 8 && w != 16 && w != 32 && w != 64)
    fail("element width", w);
}

uint32_t storageBytes(uint32_t w) { return w == 1 ? 1 : w / 8; }

Bit *bits(const void *b) { return (Bit *)b; }
Integer *ints(const void *b) { return (Integer *)b; }

// Reads element k of a public array in the native layout, zero-extended.
uint64_t readPub(const void *pub, uint32_t w, uint64_t k) {
  const uint8_t *p = (const uint8_t *)pub + k * storageBytes(w);
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

void writePub(void *pub, uint32_t w, uint64_t k, uint64_t v) {
  uint8_t *p = (uint8_t *)pub + k * storageBytes(w);
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

Integer publicInt(uint32_t w, uint64_t v) { return Integer(w, (int64_t)v, PUBLIC); }

// Flips the sign bit, which maps unsigned order onto signed order.
Integer flipSign(Integer x) {
  x[x.size() - 1] = !x[x.size() - 1];
  return x;
}

Bit sgt(const Integer &a, const Integer &b) { return !b.geq(a); }
Bit sge(const Integer &a, const Integer &b) { return a.geq(b); }

Bit icmpInt(uint32_t pred, const Integer &a, const Integer &b) {
  switch (pred) {
  case SMAUG_ICMP_EQ:
    return a.equal(b);
  case SMAUG_ICMP_NE:
    return !a.equal(b);
  case SMAUG_ICMP_SGT:
    return sgt(a, b);
  case SMAUG_ICMP_SGE:
    return sge(a, b);
  case SMAUG_ICMP_SLT:
    return sgt(b, a);
  case SMAUG_ICMP_SLE:
    return sge(b, a);
  case SMAUG_ICMP_UGT:
    return sgt(flipSign(a), flipSign(b));
  case SMAUG_ICMP_UGE:
    return sge(flipSign(a), flipSign(b));
  case SMAUG_ICMP_ULT:
    return sgt(flipSign(b), flipSign(a));
  case SMAUG_ICMP_ULE:
    return sge(flipSign(b), flipSign(a));
  }
  fail("icmp predicate", pred);
}

// i1 values: true is 1 unsigned and -1 signed.
Bit icmpBit(uint32_t pred, const Bit &a, const Bit &b) {
  switch (pred) {
  case SMAUG_ICMP_EQ:
    return !(a ^ b);
  case SMAUG_ICMP_NE:
    return a ^ b;
  case SMAUG_ICMP_UGT:
  case SMAUG_ICMP_SLT:
    return a & !b;
  case SMAUG_ICMP_UGE:
  case SMAUG_ICMP_SLE:
    return a | !b;
  case SMAUG_ICMP_ULT:
  case SMAUG_ICMP_SGT:
    return !a & b;
  case SMAUG_ICMP_ULE:
  case SMAUG_ICMP_SGE:
    return !a | b;
  }
  fail("icmp predicate", pred);
}

enum class Op { Add, Sub, Mul, SDiv, UDiv, SRem, URem, And, Or, Xor, Shl, LShr, AShr, SMin, SMax, UMin, UMax };

Integer complement(const Integer &x) {
  return x ^ Integer(x.size(), -1, PUBLIC);
}

Integer ashrInt(const Integer &x, const Integer &s) {
  // Shifting the complement of a negative value logically and complementing
  // the result fills with ones.
  Bit neg = x[x.size() - 1];
  Integer pos = x >> s;
  Integer negRes = complement(complement(x) >> s);
  return pos.select(neg, negRes);
}

Integer ashrPub(const Integer &x, uint64_t k) {
  Integer res = x >> k;
  for (size_t i = x.size() - std::min<uint64_t>(k, x.size()); i < x.size(); ++i)
    res[i] = x[x.size() - 1];
  return res;
}

Integer unsignedDivRem(const Integer &a, const Integer &b, bool rem) {
  // One extra zero bit makes both operands non-negative for emp's signed
  // division.
  Integer x = a, y = b;
  x.resize(a.size() + 1, false);
  y.resize(b.size() + 1, false);
  Integer r = rem ? x % y : x / y;
  r.resize(a.size(), false);
  return r;
}

Integer intOp(Op op, const Integer &a, const Integer &b) {
  switch (op) {
  case Op::Add:
    return a + b;
  case Op::Sub:
    return a - b;
  case Op::Mul:
    return a * b;
  case Op::SDiv:
    return a / b;
  case Op::UDiv:
    return unsignedDivRem(a, b, false);
  case Op::SRem:
    return a % b;
  case Op::URem:
    return unsignedDivRem(a, b, true);
  case Op::And:
    return a & b;
  case Op::Or:
    return a | b;
  case Op::Xor:
    return a ^ b;
  case Op::Shl:
    return a << b;
  case Op::LShr:
    return a >> b;
  case Op::AShr:
    return ashrInt(a, b);
  case Op::SMin:
    return a.select(sgt(a, b), b);
  case Op::SMax:
    return b.select(sgt(a, b), a);
  case Op::UMin:
    return a.select(sgt(flipSign(a), flipSign(b)), b);
  case Op::UMax:
    return b.select(sgt(flipSign(a), flipSign(b)), a);
  }
  abort();
}

// Secret op public, with the public value on the right. Shifts by a public
// amount move bits without gates; other operations use emp's public
// constants.
Integer intOpPubRhs(Op op, const Integer &a, uint64_t v) {
  switch (op) {
  case Op::Shl:
    return a << v;
  case Op::LShr:
    return a >> v;
  case Op::AShr:
    return ashrPub(a, v);
  default:
    return intOp(op, a, publicInt(a.size(), v));
  }
}

// i1 semantics: add and sub are xor, mul is and, division needs a divisor of
// 1 (unsigned) or -1 (signed), so it returns the dividend; remainders are 0;
// shifts must be by 0. Signed order has true below false.
Bit bitOp(Op op, const Bit &a, const Bit &b) {
  switch (op) {
  case Op::Add:
  case Op::Sub:
  case Op::Xor:
    return a ^ b;
  case Op::Mul:
  case Op::And:
  case Op::SMax:
  case Op::UMin:
    return a & b;
  case Op::Or:
  case Op::SMin:
  case Op::UMax:
    return a | b;
  case Op::SDiv:
  case Op::UDiv:
  case Op::Shl:
  case Op::LShr:
  case Op::AShr:
    return a;
  case Op::SRem:
  case Op::URem:
    return Bit(false, PUBLIC);
  }
  abort();
}

void binop(Op op, uint32_t w, void *out, const void *a, const void *b,
           uint64_t n) {
  checkWidth(w);
  for (uint64_t i = 0; i < n; ++i) {
    if (w == 1)
      bits(out)[i] = bitOp(op, bits(a)[i], bits(b)[i]);
    else
      ints(out)[i] = intOp(op, ints(a)[i], ints(b)[i]);
  }
}

void binopPub(Op op, uint32_t w, void *out, const void *a, const void *pub,
              uint64_t pstride, uint32_t pubIsLhs, uint64_t n) {
  checkWidth(w);
  for (uint64_t i = 0; i < n; ++i) {
    uint64_t v = readPub(pub, w, i * pstride);
    if (w == 1) {
      Bit p(v & 1, PUBLIC);
      bits(out)[i] = pubIsLhs ? bitOp(op, p, bits(a)[i]) : bitOp(op, bits(a)[i], p);
    } else if (pubIsLhs) {
      ints(out)[i] = intOp(op, publicInt(w, v), ints(a)[i]);
    } else {
      ints(out)[i] = intOpPubRhs(op, ints(a)[i], v);
    }
  }
}

// Float values are 32-bit patterns in Integers.
Float toFloat(const Integer &x) {
  Float f;
  for (int i = 0; i < 32; ++i)
    f.value[i] = x[i];
  return f;
}

Integer fromFloat(const Float &f) {
  std::vector<Bit> v(f.value.begin(), f.value.end());
  return Integer(v);
}

Float publicFloat(uint64_t pattern) {
  uint32_t p = (uint32_t)pattern;
  float f;
  memcpy(&f, &p, 4);
  return Float(f, PUBLIC);
}

enum class FOp { Add, Sub, Mul, Div };

Float floatOp(FOp op, const Float &a, const Float &b) {
  switch (op) {
  case FOp::Add:
    return a + b;
  case FOp::Sub:
    return a - b;
  case FOp::Mul:
    return a * b;
  case FOp::Div:
    return a / b;
  }
  abort();
}

Bit fcmpOp(uint32_t pred, const Float &a, const Float &b) {
  switch (pred) {
  case SMAUG_FCMP_OEQ:
    return a.equal(b);
  case SMAUG_FCMP_ONE:
    return !a.equal(b);
  case SMAUG_FCMP_OLT:
    return a.less_than(b);
  case SMAUG_FCMP_OLE:
    return a.less_equal(b);
  case SMAUG_FCMP_OGT:
    return b.less_than(a);
  case SMAUG_FCMP_OGE:
    return b.less_equal(a);
  }
  fail("fcmp predicate", pred);
}

void floatBinop(FOp op, uint32_t w, void *out, const void *a, const void *b,
                uint64_t n) {
  if (w != 32)
    fail("float operation", w);
  for (uint64_t i = 0; i < n; ++i)
    ints(out)[i] = fromFloat(floatOp(op, toFloat(ints(a)[i]), toFloat(ints(b)[i])));
}

void floatBinopPub(FOp op, uint32_t w, void *out, const void *a,
                   const void *pub, uint64_t pstride, uint32_t pubIsLhs,
                   uint64_t n) {
  if (w != 32)
    fail("float operation", w);
  for (uint64_t i = 0; i < n; ++i) {
    Float p = publicFloat(readPub(pub, 32, i * pstride));
    Float x = toFloat(ints(a)[i]);
    ints(out)[i] = fromFloat(pubIsLhs ? floatOp(op, p, x) : floatOp(op, x, p));
  }
}

// Every bit's wire label, in element order, LSB first.
std::vector<block> labels(uint32_t w, const void *src, uint64_t n) {
  std::vector<block> l(n * w);
  for (uint64_t i = 0; i < n; ++i) {
    if (w == 1)
      l[i] = bits(src)[i].bit;
    else
      for (uint32_t j = 0; j < w; ++j)
        l[i * w + j] = ints(src)[i][j].bit;
  }
  return l;
}

void fromLabels(uint32_t w, void *dst, const block *l, uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) {
    if (w == 1) {
      bits(dst)[i] = Bit(l[i]);
    } else {
      std::vector<Bit> v(w);
      for (uint32_t j = 0; j < w; ++j)
        v[j] = Bit(l[i * w + j]);
      ints(dst)[i] = Integer(v);
    }
  }
}

std::unique_ptr<bool[]> toBools(uint32_t w, const void *plain, uint64_t n) {
  std::unique_ptr<bool[]> b(new bool[n * w]);
  for (uint64_t i = 0; i < n; ++i) {
    uint64_t v = readPub(plain, w, i);
    for (uint32_t j = 0; j < w; ++j)
      b[i * w + j] = (v >> j) & 1;
  }
  return b;
}

void fromBools(uint32_t w, void *plain, const bool *b, uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) {
    uint64_t v = 0;
    for (uint32_t j = 0; j < w; ++j)
      v |= (uint64_t)b[i * w + j] << j;
    writePub(plain, w, i, v);
  }
}

} // namespace

extern "C" {

void *smaug_alloc(uint32_t w, uint64_t n) {
  checkWidth(w);
  if (w == 1) {
    Bit *b = new Bit[n ? n : 1];
    for (uint64_t i = 0; i < n; ++i)
      b[i] = Bit(false, PUBLIC);
    return b;
  }
  Integer *x = new Integer[n ? n : 1];
  for (uint64_t i = 0; i < n; ++i)
    x[i] = publicInt(w, 0);
  return x;
}

void smaug_free(void *b, uint32_t w, uint64_t) {
  if (w == 1)
    delete[] bits(b);
  else
    delete[] ints(b);
}

void *smaug_elem(void *b, uint32_t w, uint64_t i) {
  return w == 1 ? (void *)(bits(b) + i) : (void *)(ints(b) + i);
}

int32_t smaug_party(void) { return MPC::party; }

void smaug_copy(uint32_t w, void *dst, const void *src, uint64_t n) {
  if (dst == src || n == 0)
    return;
  // memmove order, so overlapping ranges copy correctly.
  bool backward = dst > src;
  for (uint64_t k = 0; k < n; ++k) {
    uint64_t i = backward ? n - 1 - k : k;
    if (w == 1)
      bits(dst)[i] = bits(src)[i];
    else
      ints(dst)[i] = ints(src)[i];
  }
}

void smaug_fill(uint32_t w, void *dst, const void *src1, uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) {
    if (w == 1)
      bits(dst)[i] = bits(src1)[0];
    else
      ints(dst)[i] = ints(src1)[0];
  }
}

void smaug_reverse(uint32_t w, void *dst, const void *src, uint64_t n) {
  if (w == 1) {
    std::vector<Bit> t(bits(src), bits(src) + n);
    for (uint64_t i = 0; i < n; ++i)
      bits(dst)[i] = t[n - 1 - i];
  } else {
    std::vector<Integer> t(ints(src), ints(src) + n);
    for (uint64_t i = 0; i < n; ++i)
      ints(dst)[i] = t[n - 1 - i];
  }
}

// Each party feeds its XOR share; the secret is the XOR of the two inputs.
// Both parties feed ALICE's input first.
void smaug_import(uint32_t w, void *dst, const void *share, uint64_t n) {
  checkWidth(w);
  uint64_t m = n * w;
  std::unique_ptr<bool[]> mine = toBools(w, share, n);
  std::unique_ptr<bool[]> none(new bool[m ? m : 1]());
  std::vector<block> a(m ? m : 1), b(m ? m : 1);
  ProtocolExecution::prot_exec->feed(a.data(), ALICE,
                                     MPC::party == ALICE ? mine.get() : none.get(), m);
  ProtocolExecution::prot_exec->feed(b.data(), BOB,
                                     MPC::party == BOB ? mine.get() : none.get(), m);
  std::vector<block> x(m ? m : 1);
  for (uint64_t k = 0; k < m; ++k)
    x[k] = (Bit(a[k]) ^ Bit(b[k])).bit;
  fromLabels(w, dst, x.data(), n);
}

void smaug_export(uint32_t w, void *share, const void *src, uint64_t n) {
  checkWidth(w);
  std::vector<block> l = labels(w, src, n);
  std::unique_ptr<bool[]> out(new bool[l.size() ? l.size() : 1]);
  ProtocolExecution::prot_exec->reveal(out.get(), XOR, l.data(), l.size());
  fromBools(w, share, out.get(), n);
}

void smaug_share_public(uint32_t w, void *dst, const void *pub,
                        uint64_t pstride, uint64_t n) {
  checkWidth(w);
  for (uint64_t i = 0; i < n; ++i) {
    uint64_t v = readPub(pub, w, i * pstride);
    if (w == 1)
      bits(dst)[i] = Bit(v & 1, PUBLIC);
    else
      ints(dst)[i] = publicInt(w, v);
  }
}

void smaug_reveal(uint32_t w, void *pub, const void *src, uint64_t n) {
  checkWidth(w);
  std::vector<block> l = labels(w, src, n);
  std::unique_ptr<bool[]> out(new bool[l.size() ? l.size() : 1]);
  ProtocolExecution::prot_exec->reveal(out.get(), PUBLIC, l.data(), l.size());
  fromBools(w, pub, out.get(), n);
}

#define BINOP(name, op)                                                        \
  void smaug_##name(uint32_t w, void *out, const void *a, const void *b,       \
                    uint64_t n) {                                              \
    binop(op, w, out, a, b, n);                                                \
  }                                                                            \
  void smaug_##name##_sp(uint32_t w, void *out, const void *a,                 \
                         const void *pub, uint64_t pstride,                    \
                         uint32_t pub_is_lhs, uint64_t n) {                    \
    binopPub(op, w, out, a, pub, pstride, pub_is_lhs, n);                      \
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
  binop(Op::And, w, out, a, b, n);
}
void smaug_and_sp(uint32_t w, void *out, const void *a, const void *pub,
                  uint64_t pstride, uint32_t pub_is_lhs, uint64_t n) {
  binopPub(Op::And, w, out, a, pub, pstride, pub_is_lhs, n);
}
void smaug_or(uint32_t w, void *out, const void *a, const void *b, uint64_t n) {
  binop(Op::Or, w, out, a, b, n);
}
void smaug_or_sp(uint32_t w, void *out, const void *a, const void *pub,
                 uint64_t pstride, uint32_t pub_is_lhs, uint64_t n) {
  binopPub(Op::Or, w, out, a, pub, pstride, pub_is_lhs, n);
}
void smaug_xor(uint32_t w, void *out, const void *a, const void *b, uint64_t n) {
  binop(Op::Xor, w, out, a, b, n);
}
void smaug_xor_sp(uint32_t w, void *out, const void *a, const void *pub,
                  uint64_t pstride, uint32_t pub_is_lhs, uint64_t n) {
  binopPub(Op::Xor, w, out, a, pub, pstride, pub_is_lhs, n);
}

void smaug_icmp(uint32_t pred, uint32_t w, void *out, const void *a,
                const void *b, uint64_t n) {
  checkWidth(w);
  for (uint64_t i = 0; i < n; ++i)
    bits(out)[i] = w == 1 ? icmpBit(pred, bits(a)[i], bits(b)[i])
                          : icmpInt(pred, ints(a)[i], ints(b)[i]);
}

void smaug_icmp_sp(uint32_t pred, uint32_t w, void *out, const void *a,
                   const void *pub, uint64_t pstride, uint32_t pub_is_lhs,
                   uint64_t n) {
  checkWidth(w);
  for (uint64_t i = 0; i < n; ++i) {
    uint64_t v = readPub(pub, w, i * pstride);
    if (w == 1) {
      Bit p(v & 1, PUBLIC);
      bits(out)[i] = pub_is_lhs ? icmpBit(pred, p, bits(a)[i])
                                : icmpBit(pred, bits(a)[i], p);
    } else {
      Integer p = publicInt(w, v);
      bits(out)[i] = pub_is_lhs ? icmpInt(pred, p, ints(a)[i])
                                : icmpInt(pred, ints(a)[i], p);
    }
  }
}

void smaug_select(uint32_t w, void *out, const void *cond, const void *t,
                  const void *f, uint64_t n) {
  checkWidth(w);
  for (uint64_t i = 0; i < n; ++i) {
    const Bit &c = bits(cond)[i];
    if (w == 1)
      bits(out)[i] = bits(f)[i].select(c, bits(t)[i]);
    else
      ints(out)[i] = ints(f)[i].select(c, ints(t)[i]);
  }
}

void smaug_select_pubcond(uint32_t w, void *out, const void *pcond,
                          uint64_t pstride, const void *t, const void *f,
                          uint64_t n) {
  checkWidth(w);
  for (uint64_t i = 0; i < n; ++i) {
    bool c = readPub(pcond, 1, i * pstride);
    if (w == 1)
      bits(out)[i] = c ? bits(t)[i] : bits(f)[i];
    else
      ints(out)[i] = c ? ints(t)[i] : ints(f)[i];
  }
}

void smaug_zext(uint32_t wd, uint32_t ws, void *out, const void *a, uint64_t n) {
  checkWidth(wd);
  checkWidth(ws);
  for (uint64_t i = 0; i < n; ++i) {
    if (ws == 1) {
      std::vector<Bit> v(wd, Bit(false, PUBLIC));
      v[0] = bits(a)[i];
      ints(out)[i] = Integer(v);
    } else {
      Integer x = ints(a)[i];
      ints(out)[i] = x.resize(wd, false);
    }
  }
}

void smaug_sext(uint32_t wd, uint32_t ws, void *out, const void *a, uint64_t n) {
  checkWidth(wd);
  checkWidth(ws);
  for (uint64_t i = 0; i < n; ++i) {
    if (ws == 1) {
      ints(out)[i] = Integer(std::vector<Bit>(wd, bits(a)[i]));
    } else {
      Integer x = ints(a)[i];
      ints(out)[i] = x.resize(wd, true);
    }
  }
}

void smaug_trunc(uint32_t wd, uint32_t ws, void *out, const void *a, uint64_t n) {
  checkWidth(wd);
  checkWidth(ws);
  for (uint64_t i = 0; i < n; ++i) {
    if (wd == 1) {
      bits(out)[i] = ints(a)[i][0];
    } else {
      Integer x = ints(a)[i];
      ints(out)[i] = x.resize(wd, false);
    }
  }
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
    uint64_t ones = w == 64 ? ~0ull : ((1ull << w) - 1);
    if (o == Op::Mul) id = 1;
    if (o == Op::And || o == Op::UMin) id = ones;
    if (o == Op::SMax) id = 1ull << (w - 1);
    if (o == Op::SMin) id = ones >> 1;
    smaug_share_public(w, out1, &id, 0, 1);
    return;
  }
  if (w == 1) {
    Bit r = bits(a)[0];
    for (uint64_t i = 1; i < n; ++i)
      r = bitOp(o, r, bits(a)[i]);
    bits(out1)[0] = r;
  } else {
    Integer r = ints(a)[0];
    for (uint64_t i = 1; i < n; ++i)
      r = intOp(o, r, ints(a)[i]);
    ints(out1)[0] = r;
  }
}

void smaug_oload(uint32_t w, void *out1, const void *buf, uint64_t len,
                 const void *idx, uint32_t idx_w) {
  checkWidth(w);
  checkWidth(idx_w);
  if (w == 1) {
    Bit r(false, PUBLIC);
    for (uint64_t i = 0; i < len; ++i) {
      Bit hit = idx_w == 1 ? !(bits(idx)[0] ^ Bit(i & 1, PUBLIC)) & Bit(i < 2, PUBLIC)
                           : ints(idx)[0].equal(publicInt(idx_w, i));
      r = r.select(hit, bits(buf)[i]);
    }
    bits(out1)[0] = r;
    return;
  }
  Integer r = publicInt(w, 0);
  for (uint64_t i = 0; i < len; ++i) {
    Bit hit = idx_w == 1 ? !(bits(idx)[0] ^ Bit(i & 1, PUBLIC)) & Bit(i < 2, PUBLIC)
                         : ints(idx)[0].equal(publicInt(idx_w, i));
    r = r.select(hit, ints(buf)[i]);
  }
  ints(out1)[0] = r;
}

void smaug_ostore(uint32_t w, void *buf, uint64_t len, const void *idx,
                  uint32_t idx_w, const void *val1) {
  checkWidth(w);
  checkWidth(idx_w);
  for (uint64_t i = 0; i < len; ++i) {
    Bit hit = idx_w == 1 ? !(bits(idx)[0] ^ Bit(i & 1, PUBLIC)) & Bit(i < 2, PUBLIC)
                         : ints(idx)[0].equal(publicInt(idx_w, i));
    if (w == 1)
      bits(buf)[i] = bits(buf)[i].select(hit, bits(val1)[0]);
    else
      ints(buf)[i] = ints(buf)[i].select(hit, ints(val1)[0]);
  }
}

#define FBINOP(name, op)                                                       \
  void smaug_##name(uint32_t w, void *out, const void *a, const void *b,       \
                    uint64_t n) {                                              \
    floatBinop(op, w, out, a, b, n);                                           \
  }                                                                            \
  void smaug_##name##_sp(uint32_t w, void *out, const void *a,                 \
                         const void *pub, uint64_t pstride,                    \
                         uint32_t pub_is_lhs, uint64_t n) {                    \
    floatBinopPub(op, w, out, a, pub, pstride, pub_is_lhs, n);                 \
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
  for (uint64_t i = 0; i < n; ++i)
    bits(out)[i] = fcmpOp(pred, toFloat(ints(a)[i]), toFloat(ints(b)[i]));
}

void smaug_fcmp_sp(uint32_t pred, uint32_t w, void *out, const void *a,
                   const void *pub, uint64_t pstride, uint32_t pub_is_lhs,
                   uint64_t n) {
  if (w != 32)
    fail("float comparison", w);
  for (uint64_t i = 0; i < n; ++i) {
    Float p = publicFloat(readPub(pub, 32, i * pstride));
    Float x = toFloat(ints(a)[i]);
    bits(out)[i] = pub_is_lhs ? fcmpOp(pred, p, x) : fcmpOp(pred, x, p);
  }
}

} // extern "C"
