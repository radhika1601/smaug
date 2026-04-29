#include "emp-sh2pc/emp-sh2pc.h"
#include "emp-tool/utils/utils.h"
#include "mpc/mpc.h"
#include <fstream>
#include <iostream>

using namespace emp;

namespace MPC {
PRG prg;
int party;
vector<NetIO *> ios;
long num_ands = 0;
float time_in_mpc = 0;
int getNumGates() {
  long x = CircuitExecution::circ_exec->num_and();
  printf("time used in mpc: %f\t", time_in_mpc / 1000000);
  return x - num_ands;
}
void setNumGates() { num_ands = CircuitExecution::circ_exec->num_and(); }
void makeShared(void *arr, int n, int elementSize) {
  if (party != 1)
    memset(arr, 0, n * elementSize);
}

void send_bool(bool *b, int n) {
  ios[0]->send_bool(b, n);
  ios[0]->flush();
}
void recv_bool(bool *b, int n) { ios[0]->recv_bool(b, n); }

void reverse(void *arr, void *arr2, int n, int elementSize) {
  for (int i = 0; i < n; ++i) {
    if (log2(elementSize) == 0) {
      ((int8_t *)arr2)[i] = ((int8_t *)arr)[n - i - 1];
    } else if (log2(elementSize) == 1) {
      ((int16_t *)arr2)[i] = ((int16_t *)arr)[n - i - 1];
    } else if (log2(elementSize) == 2) {
      ((int32_t *)arr2)[i] = ((int32_t *)arr)[n - i - 1];
    } else if (log2(elementSize) == 3) {
      ((int64_t *)arr2)[i] = ((int64_t *)arr)[n - i - 1];
    }
  }
}

void store(void *arr, void *element, int n, int elementSize, bool shared,
           bool consecutive) {
  if (!shared && party != 1) {
    memset(arr, 0, n * elementSize);
    return;
  }
  // printf("element to store %hd\n", ((int8_t *)element)[0]);
  for (int i = 0; i < n; ++i) {
    if (log2(elementSize) == 0)
      ((int8_t *)arr)[i] = ((int8_t *)element)[0];
    else if (log2(elementSize) == 1)
      ((int16_t *)arr)[i] = ((int16_t *)element)[0];
    else if (log2(elementSize) == 2) {
      ((int32_t *)arr)[i] = ((int32_t *)element)[0];
    } else if (log2(elementSize) == 3)
      ((int64_t *)arr)[i] = ((int64_t *)element)[0];
  }

  if (consecutive) {
    for (int i = 0; i < n; ++i) {
      if (log2(elementSize) == 0)
        ((int8_t *)arr)[i] += i;
      else if (log2(elementSize) == 1)
        ((int16_t *)arr)[i] += i;
      else if (log2(elementSize) == 2)
        ((int32_t *)arr)[i] += i;
      else if (log2(elementSize) == 3)
        ((int64_t *)arr)[i] += i;
    }
  }
}

void updateType(void *a, void *b, int n, int elementSize1, int elementSize2) {
  for (int i = 0; i < n; ++i)
    if (log2(elementSize1) == 0) {
      for (int i = 0; i < n; ++i)
        if (log2(elementSize2) == 0)
          ((int8_t *)a)[i] = ((int8_t *)b)[i];
        else if (log2(elementSize2) == 1)
          ((int8_t *)a)[i] = ((int16_t *)b)[i];
        else if (log2(elementSize2) == 2)
          ((int8_t *)a)[i] = ((int32_t *)b)[i];
        else if (log2(elementSize2) == 3)
          ((int8_t *)a)[i] = ((int64_t *)b)[i];
    } else if (log2(elementSize1) == 1) {
      for (int i = 0; i < n; ++i)
        if (log2(elementSize2) == 0)
          ((int16_t *)a)[i] = ((int8_t *)b)[i];
        else if (log2(elementSize2) == 1)
          ((int16_t *)a)[i] = ((int16_t *)b)[i];
        else if (log2(elementSize2) == 2)
          ((int16_t *)a)[i] = ((int32_t *)b)[i];
        else if (log2(elementSize2) == 3)
          ((int16_t *)a)[i] = ((int64_t *)b)[i];
    } else if (log2(elementSize1) == 2) {
      for (int i = 0; i < n; ++i)
        if (log2(elementSize2) == 0)
          ((int32_t *)a)[i] = ((int8_t *)b)[i];
        else if (log2(elementSize2) == 1)
          ((int32_t *)a)[i] = ((int16_t *)b)[i];
        else if (log2(elementSize2) == 2)
          ((int32_t *)a)[i] = ((int32_t *)b)[i];
        else if (log2(elementSize2) == 3)
          ((int32_t *)a)[i] = ((int64_t *)b)[i];
    } else if (log2(elementSize1) == 3) {
      for (int i = 0; i < n; ++i)
        if (log2(elementSize2) == 0)
          ((int64_t *)a)[i] = ((int8_t *)b)[i];
        else if (log2(elementSize2) == 1)
          ((int64_t *)a)[i] = ((int16_t *)b)[i];
        else if (log2(elementSize2) == 2)
          ((int64_t *)a)[i] = ((int32_t *)b)[i];
        else if (log2(elementSize2) == 3)
          ((int64_t *)a)[i] = ((int64_t *)b)[i];
    }
}

template <typename T, typename Op>
__attribute__((always_inline)) void compute(T *input1, T *input2, T *output,
                                            int n) {
  auto start = clock_start();
  Integer *f[2], *t[2];
  f[0] = new Integer[n];
  f[1] = new Integer[n];
  t[0] = new Integer[n];
  t[1] = new Integer[n];
  for (int i = 0; i < n; ++i) {
    f[party - 1][i] = Integer(sizeof(T) * 8, ((T *)input1)[i], party);
    t[party - 1][i] = Integer(sizeof(T) * 8, ((T *)input2)[i], party);
  }

  int otherParty = party == 1 ? 2 : 1;
  for (int i = 0; i < n; ++i) {
    f[otherParty - 1][i] = Integer(sizeof(T) * 8, 0, otherParty);
    t[otherParty - 1][i] = Integer(sizeof(T) * 8, 0, otherParty);
  }
  for (int i = 0; i < n; ++i) {
    Integer out = Op()(f[0][i] ^ f[1][i], t[0][i] ^ t[1][i]);
    if (sizeof(T) < 4) {
      output[i] = out.reveal<int32_t>(XOR);
    } else {
      output[i] = out.reveal<T>(XOR);
    }
  }
  auto time = time_from(start);
  time_in_mpc += time;
  delete[] f[0];
  delete[] t[0];
  delete[] f[1];
  delete[] t[1];
}

void divide(void *a, void *b, int n, int elementSize, void *res, bool shared) {
  if (!shared) {
    for (int i = 0; i < n; ++i) {
      if (log2(elementSize) == 0)
        ((int8_t *)res)[i] = ((int8_t *)a)[i] / ((int8_t *)b)[i];
      else if (log2(elementSize) == 1)
        ((int16_t *)res)[i] = ((int16_t *)a)[i] / ((int16_t *)b)[i];
      else if (log2(elementSize) == 2)
        ((int32_t *)res)[i] = ((int32_t *)a)[i] / ((int32_t *)b)[i];
      else if (log2(elementSize) == 3)
        ((int64_t *)res)[i] = (int64_t)(((int64_t *)a)[i] / ((int64_t *)b)[i]);
    }
    return;
  }

  if (log2(elementSize) == 0)
    compute<int8_t, std::divides<Integer>>((int8_t *)a, (int8_t *)b,
                                           (int8_t *)res, n);
  else if (log2(elementSize) == 1)
    compute<int16_t, std::divides<Integer>>((int16_t *)a, (int16_t *)b,
                                            (int16_t *)res, n);
  else if (log2(elementSize) == 2)
    compute<int32_t, std::divides<Integer>>((int32_t *)a, (int32_t *)b,
                                            (int32_t *)res, n);
  else if (log2(elementSize) == 3)
    compute<int64_t, std::divides<Integer>>((int64_t *)a, (int64_t *)b,
                                            (int64_t *)res, n);
}

template <typename T>
__attribute__((always_inline)) void compute(T *input1, T *input2, T *output,
                                            int n, bool max = true) {
  auto start = clock_start();
  Integer *f[2], *t[2];
  f[0] = new Integer[n];
  f[1] = new Integer[n];
  t[0] = new Integer[n];
  t[1] = new Integer[n];
  for (int i = 0; i < n; ++i) {
    f[party - 1][i] = Integer(sizeof(T) * 8, ((T *)input1)[i], party);
    t[party - 1][i] = Integer(sizeof(T) * 8, ((T *)input2)[i], party);
  }

  int otherParty = party == 1 ? 2 : 1;
  for (int i = 0; i < n; ++i) {
    f[otherParty - 1][i] = Integer(sizeof(T) * 8, 0, otherParty);
    t[otherParty - 1][i] = Integer(sizeof(T) * 8, 0, otherParty);
  }
  for (int i = 0; i < n; ++i) {
    Integer out;
    Integer a = f[0][i] ^ f[1][i], b = t[0][i] ^ t[1][i];
    Bit cmp = a > b;
    if (max)
      out = b.select(cmp, a);
    else
      out = a.select(cmp, b);
    if (sizeof(T) < 4) {
      output[i] = out.reveal<int32_t>(XOR);
    } else {
      output[i] = out.reveal<T>(XOR);
    }
  }
  auto time = time_from(start);
  time_in_mpc += time;
  delete[] f[0];
  delete[] t[0];
  delete[] f[1];
  delete[] t[1];
}

template <typename Op>
__attribute__((always_inline)) void Fcompute(float *input1, float *input2,
                                             float *output, int n) {
  auto start = clock_start();
  Float *f[2], *t[2];
  f[0] = new Float[n];
  f[1] = new Float[n];
  t[0] = new Float[n];
  t[1] = new Float[n];
  for (int i = 0; i < n; ++i) {
    f[party - 1][i] = Float(input1[i], party);
    t[party - 1][i] = Float(input2[i], party);
  }

  int otherParty = party == 1 ? 2 : 1;
  for (int i = 0; i < n; ++i) {
    f[otherParty - 1][i] = Float(0, otherParty);
    t[otherParty - 1][i] = Float(0, otherParty);
  }
  for (int i = 0; i < n; ++i) {
    Float out;
    Float a = f[0][i] ^ f[1][i], b = t[0][i] ^ t[1][i];
    out = Op()(a, b);
    // Bit cmp = b.less_than(a);
    // if (max) {
    //   out = If(cmp, a, b);
    // } else
    //   out = If(cmp, b, a);

    output[i] = out.reveal<double>(XOR);
  }
  auto time = time_from(start);
  time_in_mpc += time;
  delete[] f[0];
  delete[] t[0];
  delete[] f[1];
  delete[] t[1];
}
void max(void *a, void *b, int n, int elementSize, void *res, bool shared) {
  if (!shared) {
    for (int i = 0; i < n; ++i) {
      if (log2(elementSize) == 0)
        ((int8_t *)res)[i] = ((int8_t *)a)[i] > ((int8_t *)b)[i]
                                 ? ((int8_t *)a)[i]
                                 : ((int8_t *)b)[i];
      else if (log2(elementSize) == 1)
        ((int16_t *)res)[i] = ((int16_t *)a)[i] > ((int16_t *)b)[i]
                                  ? ((int16_t *)a)[i]
                                  : ((int16_t *)b)[i];
      else if (log2(elementSize) == 2)
        ((int32_t *)res)[i] = ((int32_t *)a)[i] > ((int32_t *)b)[i]
                                  ? ((int32_t *)a)[i]
                                  : ((int32_t *)b)[i];
      else if (log2(elementSize) == 3)
        ((int64_t *)res)[i] = ((int64_t *)a)[i] > ((int64_t *)b)[i]
                                  ? ((int64_t *)a)[i]
                                  : ((int64_t *)b)[i];
    }
    return;
  }

  if (log2(elementSize) == 0)
    compute<int8_t>((int8_t *)a, (int8_t *)b, (int8_t *)res, n, true);
  else if (log2(elementSize) == 1)
    compute<int16_t>((int16_t *)a, (int16_t *)b, (int16_t *)res, n, true);
  else if (log2(elementSize) == 2)
    compute<int32_t>((int32_t *)a, (int32_t *)b, (int32_t *)res, n, true);
  else if (log2(elementSize) == 3)
    compute<int64_t>((int64_t *)a, (int64_t *)b, (int64_t *)res, n, true);
}

void min(void *a, void *b, int n, int elementSize, void *res, bool shared) {
  if (!shared) {
    for (int i = 0; i < n; ++i) {
      if (log2(elementSize) == 0)
        ((int8_t *)res)[i] = ((int8_t *)a)[i] > ((int8_t *)b)[i]
                                 ? ((int8_t *)a)[i]
                                 : ((int8_t *)b)[i];
      else if (log2(elementSize) == 1)
        ((int16_t *)res)[i] = ((int16_t *)a)[i] > ((int16_t *)b)[i]
                                  ? ((int16_t *)a)[i]
                                  : ((int16_t *)b)[i];
      else if (log2(elementSize) == 2)
        ((int32_t *)res)[i] = ((int32_t *)a)[i] > ((int32_t *)b)[i]
                                  ? ((int32_t *)a)[i]
                                  : ((int32_t *)b)[i];
      else if (log2(elementSize) == 3)
        ((int64_t *)res)[i] = ((int64_t *)a)[i] > ((int64_t *)b)[i]
                                  ? ((int64_t *)a)[i]
                                  : ((int64_t *)b)[i];
    }
    return;
  }

  if (log2(elementSize) == 0)
    compute<int8_t>((int8_t *)a, (int8_t *)b, (int8_t *)res, n, false);
  else if (log2(elementSize) == 1)
    compute<int16_t>((int16_t *)a, (int16_t *)b, (int16_t *)res, n, false);
  else if (log2(elementSize) == 2)
    compute<int32_t>((int32_t *)a, (int32_t *)b, (int32_t *)res, n, false);
  else if (log2(elementSize) == 3)
    compute<int64_t>((int64_t *)a, (int64_t *)b, (int64_t *)res, n, false);
}

template <typename T>
__attribute__((always_inline)) void redHelper(T *arr, T *res, int n, int op) {

  if (op == reductionOp::ADD)
    res[0] = arr[0];
  else if (op == reductionOp::MUL)
    res[0] = arr[0];
  else if (op == reductionOp::MAX)
    res[0] = arr[0];
  else if (op == reductionOp::MIN)
    res[0] = arr[0];
  else if (op == reductionOp::OR)
    res[0] = arr[0];
  for (int i = 1; i < n; ++i) {
    if (op == reductionOp::ADD)
      res[0] += arr[i];
    else if (op == reductionOp::MUL)
      res[0] *= arr[i];
    else if (op == reductionOp::MAX)
      res[0] = (res[0] > arr[i] ? res[0] : arr[i]);
    else if (op == reductionOp::MIN)
      res[0] = (res[0] < arr[i] ? res[0] : arr[i]);
    else if (op == reductionOp::OR)
      res[0] |= arr[0];
  }
}

void select(void *trueVal, void *falseVal, bool *condition, int n,
            int elementSize, void *res, bool shared) {
  if (!shared) {
    for (int i = 0; i < n; ++i) {
      if (elementSize == 0)
        ((bool *)res)[i] =
            condition[i] ? ((bool *)trueVal)[i] : ((bool *)falseVal)[i];
      else if (log2(elementSize) == 0)
        ((int8_t *)res)[i] =
            condition[i] ? ((int8_t *)trueVal)[i] : ((int8_t *)falseVal)[i];
      else if (log2(elementSize) == 1)
        ((int16_t *)res)[i] =
            condition[i] ? ((int16_t *)trueVal)[i] : ((int16_t *)falseVal)[i];
      else if (log2(elementSize) == 2)
        ((int32_t *)res)[i] =
            condition[i] ? ((int32_t *)trueVal)[i] : ((int32_t *)falseVal)[i];
      else if (log2(elementSize) == 3)
        ((int64_t *)res)[i] =
            condition[i] ? ((int64_t *)trueVal)[i] : ((int64_t *)falseVal)[i];
    }
    return;
  }

  //  condition & (trueVal ^ falseVal) ^ falseVal
  // setup_semi_honest(ios[0], party);
  auto start = clock_start();
  if (elementSize == 0) {
    Bit *f[2], *t[2], *c[2];
    f[0] = new Bit[n];
    f[1] = new Bit[n];
    t[0] = new Bit[n];
    t[1] = new Bit[n];
    c[0] = new Bit[n];
    c[1] = new Bit[n];

    for (int i = 0; i < n; ++i) {
      f[party - 1][i] = Bit(((bool *)falseVal)[i], party);
      t[party - 1][i] = Bit(((bool *)trueVal)[i], party);
      c[party - 1][i] = Bit(condition[i], party);
    }

    int otherParty = party == 1 ? 2 : 1;
    for (int i = 0; i < n; ++i) {
      f[otherParty - 1][i] = Bit(0, otherParty);
      t[otherParty - 1][i] = Bit(0, otherParty);
      c[otherParty - 1][i] = Bit(0, otherParty);
    }
    for (int i = 0; i < n; ++i) {
      Bit sel =
          (f[0][i] ^ f[1][i]).select(c[0][i] ^ c[1][i], t[0][i] ^ t[1][i]);
      ((bool *)res)[i] = sel.reveal(XOR);
    }
  } else {

    Integer *f[2], *t[2];
    f[0] = new Integer[n];
    f[1] = new Integer[n];
    t[0] = new Integer[n];
    t[1] = new Integer[n];
    Bit *c[2];
    c[0] = new Bit[n];
    c[1] = new Bit[n];
    for (int i = 0; i < n; ++i) {
      if (elementSize == 1) {
        f[party - 1][i] = Integer(8, ((int8_t *)falseVal)[i], party);
        t[party - 1][i] = Integer(8, ((int8_t *)trueVal)[i], party);
      } else if (elementSize == 2) {
        f[party - 1][i] = Integer(16, ((int16_t *)falseVal)[i], party);
        t[party - 1][i] = Integer(16, ((int16_t *)trueVal)[i], party);
      } else if (elementSize == 4) {
        f[party - 1][i] = Integer(32, ((int32_t *)falseVal)[i], party);
        t[party - 1][i] = Integer(32, ((int32_t *)trueVal)[i], party);
      } else if (elementSize == 8) {
        f[party - 1][i] = Integer(64, ((int64_t *)falseVal)[i], party);
        t[party - 1][i] = Integer(64, ((int64_t *)trueVal)[i], party);
      }
      c[party - 1][i] = Bit(condition[i], party);
    }

    int otherParty = party == 1 ? 2 : 1;
    for (int i = 0; i < n; ++i) {
      f[otherParty - 1][i] = Integer(elementSize * 8, 0, otherParty);
      t[otherParty - 1][i] = Integer(elementSize * 8, 0, otherParty);
      c[otherParty - 1][i] = Bit(0, otherParty);
    }
    for (int i = 0; i < n; ++i) {
      Integer sel =
          (f[0][i] ^ f[1][i]).select(c[0][i] ^ c[1][i], t[0][i] ^ t[1][i]);
      if (elementSize == 4)
        ((int32_t *)res)[i] = sel.reveal<int32_t>(XOR);
      else if (elementSize == 8)
        ((int64_t *)res)[i] = sel.reveal<int64_t>(XOR);
      else {
        printf("select for %d elementsize not implemented\n", elementSize);
        exit(-1);
      }
    }
  }
  auto time = time_from(start);
  time_in_mpc += time;
  // finalize_semi_honest();
}

void reduction(void *arr, int n, int elementSize, void *res, int op,
               bool shared) {
  if (!shared) {
    if (elementSize == 0)
      redHelper<bool>((bool *)arr, (bool *)res, n, op);
    else if (log2(elementSize) == 0)
      redHelper<int8_t>((int8_t *)arr, (int8_t *)res, n, op);
    else if (log2(elementSize) == 1)
      redHelper<int16_t>((int16_t *)arr, (int16_t *)res, n, op);
    else if (log2(elementSize) == 2)
      redHelper<int32_t>((int32_t *)arr, (int32_t *)res, n, op);
    else if (log2(elementSize) == 3)
      redHelper<int64_t>((int64_t *)arr, (int64_t *)res, n, op);
    return;
  }
  if (elementSize == 0) {
    Bit *a = new Bit[n];
    Bit *b = new Bit[n];
    if (party == ALICE) {
      for (int i = 0; i < n; ++i) {
        a[i] = Bit(((bool *)arr)[i], ALICE);
        b[i] = Bit(0, BOB);
      }
    } else {
      for (int i = 0; i < n; ++i) {
        b[i] = Bit(((bool *)arr)[i], BOB);
        a[i] = Bit(0, ALICE);
      }
    }
    Bit result = a[0] ^ b[0];
    for (int i = 1; i < n; ++i) {
      if (op == reductionOp::OR) {
        result = result | (a[i] ^ b[i]);
      } else {
        std::cout << "reduction for elementsize " << 0 << " op " << op
                  << " not implemented\n";
        exit(-1);
      }
    }
    ((bool *)res)[0] = result.reveal(XOR);
    return;
  }
  // 1 => 8, 2 => 16, 4 => 32, 8 =>64
  if (elementSize != 4 && elementSize != 8 && elementSize != 2 &&
      elementSize != 1) {
    std::cout << "reduction for elementsize " << elementSize
              << "not implemented\n";
    exit(-1);
  }
  // setup_semi_honest(ios[0], party);
  auto start = clock_start();
  Integer *a = new Integer[n];
  Integer *b = new Integer[n];
  if (party == ALICE) {
    for (int i = 0; i < n; ++i) {
      if (log2(elementSize) == 2)
        a[i] = Integer(elementSize * 8, ((int32_t *)arr)[i], ALICE);
      else if (log2(elementSize) == 3)
        a[i] = Integer(elementSize * 8, ((int64_t *)arr)[i], ALICE);

      b[i] = Integer(elementSize * 8, 0, BOB);
    }
  } else {
    for (int i = 0; i < n; ++i) {
      a[i] = Integer(elementSize * 8, 0, ALICE);

      if (log2(elementSize) == 2)
        b[i] = Integer(elementSize * 8, ((int32_t *)arr)[i], BOB);
      else if (log2(elementSize) == 3)
        b[i] = Integer(elementSize * 8, ((int64_t *)arr)[i], BOB);
    }
  }
  Integer result = a[0] ^ b[0];
  for (int i = 1; i < n; ++i) {
    if (op == reductionOp::ADD) {
      result = result + (a[i] ^ b[i]);
    } else if (op == reductionOp::MUL) {
      result = result * (a[i] ^ b[i]);
    } else if (op == reductionOp::MAX) {
      result = result.select(result < (a[i] ^ b[i]), a[i] ^ b[i]);
    } else if (op == reductionOp::MIN) {
      result = result.select(result > (a[i] ^ b[i]), a[i] ^ b[i]);
    } else if (op == reductionOp::OR) {
      result = result | (a[i] ^ b[i]);
    }
  }
  if (elementSize == 1)
    ((int8_t *)res)[0] = result.reveal<int32_t>(XOR);
  else if (elementSize == 2)
    ((int16_t *)res)[0] = result.reveal<int32_t>(XOR);
  else if (elementSize == 4)
    ((int32_t *)res)[0] = result.reveal<int32_t>(XOR);
  else if (elementSize == 8)
    ((int64_t *)res)[0] = result.reveal<int64_t>(XOR);

  auto time = time_from(start);
  time_in_mpc += time;
  // finalize_semi_honest();
}

int rand_int32() {
  bool *b = new bool[32];
  prg.random_bool(b, 32);
  int x = bool_to_int<int32_t>(b);
  return x;
}
int random(int bitlength) {
  bool *b = new bool[32];
  memset(b, 0, 32);
  prg.random_bool(b, bitlength);
  return bool_to_int<int32_t>(b);
}

template <bool> bool *reveal(bool *data, int num, int p) {
  if (party == p) {
    bool *dataRecv = (bool *)malloc(num * sizeof(bool));
    ios[0]->recv_bool(dataRecv, num);
    for (int i = 0; i < num; ++i) {
      data[i] ^= dataRecv[i];
    }
    free(dataRecv);
  } else {
    ios[0]->send_bool(data, num);
    ios[0]->flush();
  }

  return data;
}
template <typename T> T *reveal(T *data, int num, int p) {
  if (party == p) {
    T *dataRecv = (T *)malloc(num * sizeof(T));
    ios[0]->recv_data(dataRecv, num * sizeof(T));
    for (int i = 0; i < num; ++i) {
      data[i] ^= dataRecv[i];
    }
    free(dataRecv);
  } else {
    ios[0]->send_data(data, num * sizeof(T));
    ios[0]->flush();
  }

  return data;
}
template bool *reveal<bool>(bool *data, int num, int p);
template int8_t *reveal<int8_t>(int8_t *data, int num, int p);
template int16_t *reveal<int16_t>(int16_t *data, int num, int p);
template int32_t *reveal<int32_t>(int32_t *data, int num, int p);
template int64_t *reveal<int64_t>(int64_t *data, int num, int p);
template uint32_t *reveal<uint32_t>(uint32_t *data, int num, int p);

template <typename T> T reveal(T data, int p) {
  T rev;
  if (party == ALICE) {
    ios[0]->send_data(&data, sizeof(T));
    ios[0]->flush();
    ios[0]->recv_data(&rev, sizeof(T));
  } else {
    ios[0]->recv_data(&rev, sizeof(T));
    ios[0]->send_data(&data, sizeof(T));
    ios[0]->flush();
  }
  rev ^= data;
  return rev;
}

template bool reveal<bool>(bool data, int p);
template int8_t reveal<int8_t>(int8_t data, int p);
template int16_t reveal<int16_t>(int16_t data, int p);
template int32_t reveal<int32_t>(int32_t data, int p);
template int64_t reveal<int64_t>(int64_t data, int p);

bool isAlice() { return party == 1; }

void setup() {
  int port;
  std::cout << "party \n";
  std::cin >> party;
  std::cout << "port \n";
  std::cin >> port;
  setup(party, port);
}

void setup(int p, int port) {
  MPC::party = p;
  int threads = 1;
  for (int i = 0; i < threads; ++i)
    ios.push_back(
        new NetIO(party == ALICE ? nullptr : "127.0.0.1", port, true));
  setup_semi_honest(ios[0], party);
}

void loadStoreHelper(Integer *vals, int32_t idx, int32_t n,
                     Integer *andOutput) {
  Integer f[2];
  f[party - 1] = Integer(32, idx, party);
  int otherParty = party == 1 ? 2 : 1;
  f[otherParty - 1] = Integer(32, 0, otherParty);
  f[0] = f[1] ^ f[0];

  Integer zero = Integer(vals[0].bits.size(), 0, PUBLIC);
  Bit eq;
  for (int i = 0; i < n; ++i) {
    eq = (f[0] == Integer(32, i, PUBLIC));
    andOutput[i] = zero.select(eq, vals[i]);
  }
}

void load(void *arr, int32_t idx, void *res, int n, int elementSize,
          bool isPublic = false) {
  Integer *a[2];
  a[0] = new Integer[n];
  a[1] = new Integer[n];
  for (int i = 0; i < n; ++i) {
    if (log2(elementSize) == 0)
      a[party - 1][i] = Integer(elementSize * 8, ((int8_t *)arr)[i], party);
    else if (log2(elementSize) == 1)
      a[party - 1][i] = Integer(elementSize * 8, ((int16_t *)arr)[i], party);
    else if (log2(elementSize) == 2)
      a[party - 1][i] = Integer(elementSize * 8, ((int32_t *)arr)[i], party);
    else if (log2(elementSize) == 3)
      a[party - 1][i] = Integer(elementSize * 8, ((int64_t *)arr)[i], party);
  }

  int otherParty = party == 1 ? 2 : 1;
  for (int i = 0; i < n; ++i) {
    a[otherParty - 1][i] = Integer(elementSize * 8, 0, otherParty);
  }
  for (int i = 0; i < n; ++i)
    a[0][i] = a[0][i] ^ a[1][i];
  Integer *andOutput = a[1];
  if (log2(elementSize) == 0)
    loadStoreHelper(a[0], idx, n, andOutput);
  else if (log2(elementSize) == 1)
    loadStoreHelper(a[0], idx, n, andOutput);
  else if (log2(elementSize) == 2)
    loadStoreHelper(a[0], idx, n, andOutput);
  else if (log2(elementSize) == 3)
    loadStoreHelper(a[0], idx, n, andOutput);

  Integer output = andOutput[0];
  for (int i = 1; i < n; ++i)
    output ^= andOutput[i];

  if (log2(elementSize) == 0)
    ((int8_t *)res)[0] = output.reveal<int32_t>(XOR);
  else if (log2(elementSize) == 1)
    ((int16_t *)res)[0] = output.reveal<int32_t>(XOR);
  else if (log2(elementSize) == 2)
    ((int32_t *)res)[0] = output.reveal<int32_t>(XOR);
  else if (log2(elementSize) == 3)
    ((int64_t *)res)[0] = output.reveal<int64_t>(XOR);
  delete[] andOutput;
  delete[] a[0];
}

void store(void *arr, void *val, int32_t idx, int32_t n, int32_t elementSize,
           bool isPublic) {

  Integer *a[2], v[2];
  a[0] = new Integer[n];
  a[1] = new Integer[n];
  for (int i = 0; i < n; ++i) {
    if (log2(elementSize) == 0) {
      a[party - 1][i] = Integer(elementSize * 8, ((int8_t *)arr)[i], party);
      v[party - 1] = Integer(elementSize * 8, ((int8_t *)val)[0], party);
    } else if (log2(elementSize) == 1) {
      a[party - 1][i] = Integer(elementSize * 8, ((int16_t *)arr)[i], party);
      v[party - 1] = Integer(elementSize * 8, ((int16_t *)val)[0], party);
    } else if (log2(elementSize) == 2) {
      a[party - 1][i] = Integer(elementSize * 8, ((int32_t *)arr)[i], party);
      v[party - 1] = Integer(elementSize * 8, ((int32_t *)val)[0], party);
    } else if (log2(elementSize) == 3) {
      a[party - 1][i] = Integer(elementSize * 8, ((int64_t *)arr)[i], party);
      v[party - 1] = Integer(elementSize * 8, ((int64_t *)val)[0], party);
    }
  }

  int otherParty = party == 1 ? 2 : 1;
  for (int i = 0; i < n; ++i) {
    a[otherParty - 1][i] = Integer(elementSize * 8, 0, otherParty);
  }
  v[otherParty - 1] = Integer(elementSize * 8, 0, otherParty);
  v[0] = v[0] ^ v[1];
  for (int i = 0; i < n; ++i) {
    a[0][i] = a[0][i] ^ a[1][i];
    a[1][i] = a[0][i] - v[0];
  }
  if (log2(elementSize) == 0)
    loadStoreHelper(a[1], idx, n, a[1]);
  else if (log2(elementSize) == 1)
    loadStoreHelper(a[1], idx, n, a[1]);
  else if (log2(elementSize) == 2)
    loadStoreHelper(a[1], idx, n, a[1]);
  else if (log2(elementSize) == 3)
    loadStoreHelper(a[1], idx, n, a[1]);

  for (int i = 0; i < n; ++i) {
    a[0][i] = a[0][i] + a[1][i];
  }

  for (int i = 0; i < n; ++i)
    if (log2(elementSize) == 0)
      ((int8_t *)arr)[i] = a[0][i].reveal<int32_t>(XOR);
    else if (log2(elementSize) == 1)
      ((int16_t *)arr)[i] = a[0][i].reveal<int32_t>(XOR);
    else if (log2(elementSize) == 2)
      ((int32_t *)arr)[i] = a[0][i].reveal<int32_t>(XOR);
    else if (log2(elementSize) == 3)
      ((int64_t *)arr)[i] = a[0][i].reveal<int64_t>(XOR);

  delete[] a[0];
  delete[] a[1];
}

void storeConst(void *a, int8_t *b, int32_t n, int m) {
  int8_t *tmp = (int8_t *)a;
  for (int i = 0; i < n / m; ++i) {
    for (int j = 0; j < m; ++j) {
      tmp[i * m + j] = b[j];
    }
  }
}

void finish() { finalize_semi_honest(); }

void andBool(bool *input1, bool *input2, bool *output, int n,
             bool bothPrivate) {
  if (!bothPrivate) {
    for (int i = 0; i < n; ++i) {
      output[i] = input1[i] & input2[i];
    }
  } else {
    auto start = clock_start();
    Bit *f[2], *t[2];
    f[0] = new Bit[n];
    f[1] = new Bit[n];
    t[0] = new Bit[n];
    t[1] = new Bit[n];
    for (int i = 0; i < n; ++i) {
      f[party - 1][i] = Bit(((bool *)input1)[i], party);
      t[party - 1][i] = Bit(((bool *)input2)[i], party);
    }

    int otherParty = party == 1 ? 2 : 1;
    for (int i = 0; i < n; ++i) {
      f[otherParty - 1][i] = Bit(0, otherParty);
      t[otherParty - 1][i] = Bit(0, otherParty);
    }

    for (int i = 0; i < n; ++i) {
      output[i] = ((f[0][i] ^ f[1][i]) & (t[0][i] ^ t[1][i])).reveal(XOR);
    }
    auto time = time_from(start);
    time_in_mpc += time;
  }
}
void andI8(int8_t *input1, int8_t *input2, int8_t *output, int n,
           bool bothPrivate) {
  if (!bothPrivate) {
    for (int i = 0; i < n; ++i) {
      output[i] = input1[i] & input2[i];
    }
  } else
    compute<int8_t, std::bit_and<Integer>>(input1, input2, output, n);
}
void andI16(int16_t *input1, int16_t *input2, int16_t *output, int n,
            bool bothPrivate) {
  if (!bothPrivate) {
    for (int i = 0; i < n; ++i) {
      output[i] = input1[i] & input2[i];
    }
  } else
    compute<int16_t, std::bit_and<Integer>>(input1, input2, output, n);
}
void andI32(int32_t *input1, int32_t *input2, int32_t *output, int n,
            bool bothPrivate) {
  if (!bothPrivate) {
    for (int i = 0; i < n; ++i) {
      output[i] = input1[i] & input2[i];
    }
  } else
    compute<int32_t, std::bit_and<Integer>>(input1, input2, output, n);
}
void andI64(int64_t *input1, int64_t *input2, int64_t *output, int n,
            bool bothPrivate) {
  if (!bothPrivate) {
    for (int i = 0; i < n; ++i) {
      output[i] = input1[i] & input2[i];
    }
  } else
    compute<int64_t, std::bit_and<Integer>>(input1, input2, output, n);
}

void xorBool(bool *input1, bool *input2, bool *output, int n, bool bothSame) {
  if (party == 2 && !bothSame)
    return;
  auto start = clock_start();

  for (int i = 0; i < n; ++i)
    output[i] = input1[i] ^ input2[i];
  auto time = time_from(start);
  time_in_mpc += time;
}
void xorI8(int8_t *input1, int8_t *input2, int8_t *output, int n,
           bool bothSame) {
  auto start = clock_start();
  if (party == 2 && !bothSame)
    return;

  for (int i = 0; i < n; ++i)
    output[i] = input1[i] ^ input2[i];

  auto time = time_from(start);
  time_in_mpc += time;
}
void xorI16(int16_t *input1, int16_t *input2, int16_t *output, int n,
            bool bothSame) {
  if (party == 2 && !bothSame)
    return;
  auto start = clock_start();
  for (int i = 0; i < n; ++i)
    output[i] = input1[i] ^ input2[i];
  auto time = time_from(start);
  time_in_mpc += time;
}
void xorI32(int32_t *input1, int32_t *input2, int32_t *output, int n,
            bool bothSame) {
  if (party == 2 && !bothSame) {
    std::cout << "both not same\n";
    return;
  }
  auto start = clock_start();
  for (int i = 0; i < n; ++i)
    output[i] = input1[i] ^ input2[i];
  auto time = time_from(start);
  time_in_mpc += time;
}
void xorI64(int64_t *input1, int64_t *input2, int64_t *output, int n,
            bool bothSame) {
  if (party == 2 && !bothSame)
    return;
  auto start = clock_start();
  for (int i = 0; i < n; ++i)
    output[i] = input1[i] ^ input2[i];
  auto time = time_from(start);
  time_in_mpc += time;
}

void divF(float *input1, float *input2, float *output, int n) {
  Fcompute<std::divides<Float>>(input1, input2, output, n);
}

void addI8(int8_t *input1, int8_t *input2, int8_t *output, int n) {
  compute<int8_t, std::plus<Integer>>(input1, input2, output, n);
}
void addI16(int16_t *input1, int16_t *input2, int16_t *output, int n) {
  compute<int16_t, std::plus<Integer>>(input1, input2, output, n);
}
void addI32(int32_t *input1, int32_t *input2, int32_t *output, int n) {
  compute<int32_t, std::plus<Integer>>(input1, input2, output, n);
}
void addI64(int64_t *input1, int64_t *input2, int64_t *output, int n) {
  compute<int64_t, std::plus<Integer>>(input1, input2, output, n);
}
void addF(float *input1, float *input2, float *output, int n) {
  Fcompute<std::plus<Float>>(input1, input2, output, n);
}

void subI8(int8_t *input1, int8_t *input2, int8_t *output, int n) {
  compute<int8_t, std::minus<Integer>>(input1, input2, output, n);
}
void subI16(int16_t *input1, int16_t *input2, int16_t *output, int n) {
  compute<int16_t, std::minus<Integer>>(input1, input2, output, n);
}
void subI32(int32_t *input1, int32_t *input2, int32_t *output, int n) {
  compute<int32_t, std::minus<Integer>>(input1, input2, output, n);
}
void subI64(int64_t *input1, int64_t *input2, int64_t *output, int n) {
  compute<int64_t, std::minus<Integer>>(input1, input2, output, n);
}
void subF(float *input1, float *input2, float *output, int n) {
  Fcompute<std::minus<Float>>(input1, input2, output, n);
}

void multI8(int8_t *input1, int8_t *input2, int8_t *output, int n) {
  compute<int8_t, std::multiplies<Integer>>(input1, input2, output, n);
}
void multI16(int16_t *input1, int16_t *input2, int16_t *output, int n) {
  compute<int16_t, std::multiplies<Integer>>(input1, input2, output, n);
}
void multI32(int32_t *input1, int32_t *input2, int32_t *output, int n) {
  compute<int32_t, std::multiplies<Integer>>(input1, input2, output, n);
}
void multI64(int64_t *input1, int64_t *input2, int64_t *output, int n) {
  compute<int64_t, std::multiplies<Integer>>(input1, input2, output, n);
}
void multF(float *input1, float *input2, float *output, int n) {
  Fcompute<std::multiplies<Float>>(input1, input2, output, n);
}

template <typename T>
__attribute__((always_inline)) void computeIcmp(T *input1, T *input2,
                                                bool *output, int op, int n) {
  auto start = clock_start();

  Integer *f[2], *t[2];
  f[0] = new Integer[n];
  f[1] = new Integer[n];
  t[0] = new Integer[n];
  t[1] = new Integer[n];
  for (int i = 0; i < n; ++i) {
    f[party - 1][i] = Integer(sizeof(T) * 8, ((T *)input1)[i], party);
    t[party - 1][i] = Integer(sizeof(T) * 8, ((T *)input2)[i], party);
  }

  // setup_semi_honest(ios[0], party);
  int otherParty = party == 1 ? 2 : 1;
  for (int i = 0; i < n; ++i) {
    f[otherParty - 1][i] = Integer(sizeof(T) * 8, 0, otherParty);
    t[otherParty - 1][i] = Integer(sizeof(T) * 8, 0, otherParty);
  }
  for (int i = 0; i < n; ++i) {
    Bit out;
    Integer a = f[0][i] ^ f[1][i], b = t[0][i] ^ t[1][i];
    switch (op) {
    case cmp::EQ:
      out = (a == b);
      break;
    case cmp::GT:
      out = (a > b);
      break;
    case cmp::GE:
      out = (a >= b);
      break;
    case cmp::NE:
      out = (a != b);
      break;
    default:
      break;
    }
    output[i] = out.reveal(XOR);
  }
  auto time = time_from(start);
  time_in_mpc += time;
  delete[] f[0];
  delete[] t[0];
  delete[] f[1];
  delete[] t[1];
}

void icmpEqI8(int8_t *input1, int8_t *input2, bool *output, int n, int op) {
  computeIcmp<int8_t>(input1, input2, output, op, n);
}
void icmpEqI16(int16_t *input1, int16_t *input2, bool *output, int n, int op) {
  computeIcmp<int16_t>(input1, input2, output, op, n);
}
void icmpEqI32(int32_t *input1, int32_t *input2, bool *output, int n, int op) {
  computeIcmp<int32_t>(input1, input2, output, op, n);
}
void icmpEqI64(int64_t *input1, int64_t *input2, bool *output, int n, int op) {
  computeIcmp<int64_t>(input1, input2, output, op, n);
}

void fcmpEq(float *input1, float *input2, bool *output, int n, int op) {
  auto start = clock_start();
  Float *f[2], *t[2];
  f[0] = new Float[n];
  f[1] = new Float[n];
  t[0] = new Float[n];
  t[1] = new Float[n];
  for (int i = 0; i < n; ++i) {
    f[party - 1][i] = Float(input1[i], party);
    t[party - 1][i] = Float(input2[i], party);
  }

  // setup_semi_honest(ios[0], party);
  int otherParty = party == 1 ? 2 : 1;
  for (int i = 0; i < n; ++i) {
    f[otherParty - 1][i] = Float(0, otherParty);
    t[otherParty - 1][i] = Float(0, otherParty);
  }
  for (int i = 0; i < n; ++i) {
    Bit out;
    Float a = f[0][i] ^ f[1][i], b = t[0][i] ^ t[1][i];
    switch (op) {
    case cmp::EQ:
      out = a.equal(b);
      break;
    case cmp::GT:
      out = b.less_than(a);
      break;
    case cmp::GE:
      out = b.less_equal(a);
      break;
    case cmp::NE:
      out = !a.equal(b);
      break;
    default:
      break;
    }
    output[i] = out.reveal(XOR);
  }
  auto time = time_from(start);
  time_in_mpc += time;
  delete[] f[0];
  delete[] t[0];
  delete[] f[1];
  delete[] t[1];
}

bool andBool(bool input1, bool input2) {
  bool output;
  auto start = clock_start();
  Bit f[2], t[2];
  int otherParty = party == 1 ? 2 : 1;
  f[party - 1] = Bit(input1, party);
  t[party - 1] = Bit(input2, party);
  f[otherParty - 1] = Bit(0, otherParty);
  t[otherParty - 1] = Bit(0, otherParty);
  output = ((f[0] ^ f[1]) & (t[0] ^ t[1])).reveal(XOR);
  auto time = time_from(start);
  time_in_mpc += time;
  return output;
}
int8_t andI8(int8_t input1, int8_t input2) {
  int8_t output;
  compute<int8_t, std::bit_and<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int16_t andI16(int16_t input1, int16_t input2) {
  int16_t output;
  compute<int16_t, std::bit_and<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int32_t andI32(int32_t input1, int32_t input2) {
  int32_t output;
  compute<int32_t, std::bit_and<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int64_t andI64(int64_t input1, int64_t input2) {
  int64_t output;
  compute<int64_t, std::bit_and<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int8_t addI8(int8_t input1, int8_t input2) {
  int8_t output;
  compute<int8_t, std::plus<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int16_t addI16(int16_t input1, int16_t input2) {
  int16_t output;
  compute<int16_t, std::plus<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int32_t addI32(int32_t input1, int32_t input2) {
  int32_t output;
  compute<int32_t, std::plus<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int64_t addI64(int64_t input1, int64_t input2) {
  int64_t output;
  compute<int64_t, std::plus<Integer>>(&input1, &input2, &output, 1);
  return output;
}
float addF(float input1, float input2) {
  Float f[2], t[2];
  f[party - 1] = Float(input1, party);
  t[party - 1] = Float(input2, party);
  int otherParty = party == 1 ? 2 : 1;
  f[otherParty - 1] = Float(0, otherParty);
  t[otherParty - 1] = Float(0, otherParty);
  Float res = (f[0] ^ f[1]) + (t[0] ^ t[1]);
  float output = res.reveal<double>(XOR);
  return output;
}

int8_t subI8(int8_t input1, int8_t input2) {
  int8_t output;
  compute<int8_t, std::minus<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int16_t subI16(int16_t input1, int16_t input2) {
  int16_t output;
  compute<int16_t, std::minus<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int32_t subI32(int32_t input1, int32_t input2) {
  int32_t output;
  compute<int32_t, std::minus<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int64_t subI64(int64_t input1, int64_t input2) {
  int64_t output;
  compute<int64_t, std::minus<Integer>>(&input1, &input2, &output, 1);
  return output;
}
float subF(float input1, float input2) {
  Float f[2], t[2];
  f[party - 1] = Float(input1, party);
  t[party - 1] = Float(input2, party);
  int otherParty = party == 1 ? 2 : 1;
  f[otherParty - 1] = Float(0, otherParty);
  t[otherParty - 1] = Float(0, otherParty);
  Float res = (f[0] ^ f[1]) - (t[0] ^ t[1]);
  float output = res.reveal<double>(XOR);
  return output;
}

int8_t multI8(int8_t input1, int8_t input2) {
  int8_t output;
  compute<int8_t, std::multiplies<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int16_t multI16(int16_t input1, int16_t input2) {
  int16_t output;
  compute<int16_t, std::multiplies<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int32_t multI32(int32_t input1, int32_t input2) {
  int32_t output;
  compute<int32_t, std::multiplies<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int64_t multI64(int64_t input1, int64_t input2) {
  int64_t output;
  compute<int64_t, std::multiplies<Integer>>(&input1, &input2, &output, 1);
  return output;
}
float multF(float input1, float input2) {
  Float f[2], t[2];
  f[party - 1] = Float(input1, party);
  t[party - 1] = Float(input2, party);
  int otherParty = party == 1 ? 2 : 1;
  f[otherParty - 1] = Float(0, otherParty);
  t[otherParty - 1] = Float(0, otherParty);
  Float res = (f[0] ^ f[1]) * (t[0] ^ t[1]);
  float output = res.reveal<double>(XOR);
  return output;
}

int8_t divI8(int8_t input1, int8_t input2) {
  int8_t output;
  compute<int8_t, std::divides<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int16_t divI16(int16_t input1, int16_t input2) {
  int16_t output;
  compute<int16_t, std::divides<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int32_t divI32(int32_t input1, int32_t input2) {
  int32_t output;
  compute<int32_t, std::divides<Integer>>(&input1, &input2, &output, 1);
  return output;
}
int64_t divI64(int64_t input1, int64_t input2) {
  int64_t output;
  compute<int64_t, std::divides<Integer>>(&input1, &input2, &output, 1);
  return output;
}
float divF(float input1, float input2) {
  Float f[2], t[2];
  f[party - 1] = Float(input1, party);
  t[party - 1] = Float(input2, party);
  int otherParty = party == 1 ? 2 : 1;
  f[otherParty - 1] = Float(0, otherParty);
  t[otherParty - 1] = Float(0, otherParty);
  Float res = (f[0] ^ f[1]) / (t[0] ^ t[1]);
  float output = res.reveal<double>(XOR);
  return output;
}

bool icmpEqI8(int8_t input1, int8_t input2, int op) {
  bool output;
  computeIcmp<int8_t>(&input1, &input2, &output, op, 1);
  return output;
}
bool icmpEqI16(int16_t input1, int16_t input2, int op) {
  bool output;
  computeIcmp<int16_t>(&input1, &input2, &output, op, 1);
  return output;
}
bool icmpEqI32(int32_t input1, int32_t input2, int op) {
  bool output;
  computeIcmp<int32_t>(&input1, &input2, &output, op, 1);
  return output;
}
bool icmpEqI64(int64_t input1, int64_t input2, int op) {
  bool output;
  computeIcmp<int64_t>(&input1, &input2, &output, op, 1);
  return output;
}
bool fcmpEq(float input1, float input2, int op) {
  bool output;
  Float f[2], t[2];
  f[party - 1] = Float(input1, party);
  t[party - 1] = Float(input2, party);
  int otherParty = party == 1 ? 2 : 1;
  f[otherParty - 1] = Float(0, otherParty);
  t[otherParty - 1] = Float(0, otherParty);
  Bit out;
  Float a = f[0] ^ f[1], b = t[0] ^ t[1];
  switch (op) {
  case cmp::EQ:
    out = a.equal(b);
    break;
  case cmp::GT:
    out = b.less_than(a);
    break;
  case cmp::GE:
    out = b.less_equal(a);
    break;
  case cmp::NE:
    out = !a.equal(b);
    break;
  default:
    break;
  }
  output = out.reveal(XOR);
  return output;
}

} // namespace MPC
