#include "mpc/mpc.h"
#include "emp-aby/mp-circuit.hpp"
#include "emp-aby/simd_interface/simd_exec.h"
#include "emp-tool/utils/utils.h"
#include <fstream>
#include <iostream>

namespace MPC {
PRG prg;
int party;
int prev_depth;
vector<NetIO *> ios;
emp::SIMDCircExec<NetIO> *simd_circ;
Circuit<SIMDCircExec<NetIO>> *adder[6], *mult[6], *eq[6], *gt[6], *ge[6],
    *div[6], *sub[6];

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

int getNumGates() {
  printf(" Depth: %ld\n", simd_circ->depth);
  return simd_circ->num_and_gates;
}
void setNumGates() {
  simd_circ->num_and_gates = 0;
  simd_circ->depth = 0;
}
void makeShared(void *arr, int n, int elementSize) {
  if (party != 1)
    memset(arr, 0, n * elementSize);
}

void finish() {
  for (int i = 0; i < 4; ++i) {
    if (adder[i])
      delete adder[i];
    if (mult[i])
      delete mult[i];
    if (eq[i])
      delete eq[i];
    if (gt[i])
      delete gt[i];
    if (ge[i])
      delete ge[i];
    if (div[i])
      delete div[i];
  }

  delete simd_circ;
  for (auto io : ios) {
    delete io;
  }
}

template <typename T>
inline void compute_min_max(T *input1, T *input2, T *res, int n,
                            int max = false) {
  bool *i1 = new bool[sizeof(T) * 8 * 2 * n];
  bool *o = new bool[8 * n];
  for (int i = 0; i < n; ++i) {
    int_to_bool<T>(i1 + i * sizeof(T) * 8, input1[i], sizeof(T) * 8);
    int_to_bool<T>(i1 + n * sizeof(T) * 8 + i * sizeof(T) * 8, input2[i],
                   sizeof(T) * 8);
  }
  if (gt[(int)log2(sizeof(T))] == nullptr) {
    string name = "/usr/local/include/mpc/circuits/gt";
    name += std::to_string(sizeof(T) * 8) + ".txt";
    gt[(int)log2(sizeof(T))] =
        new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
  }
  gt[(int)log2(sizeof(T))]->compute<NetIO>(o, i1, n, true);
  bool *condition = new bool[n];
  for (int i = 0; i < n; ++i) {
    int8_t x = bool_to_int<int8_t>(o + i * 8);
    if (x == 0)
      condition[i] = false;
    else
      condition[i] = true;
  }
  if (max) {
    select(input1, input2, condition, n, sizeof(T), res, true);
  } else
    select(input2, input1, condition, n, sizeof(T), res, true);
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
    compute_min_max<int8_t>((int8_t *)a, (int8_t *)b, (int8_t *)res, n, true);
  else if (log2(elementSize) == 1)
    compute_min_max<int16_t>((int16_t *)a, (int16_t *)b, (int16_t *)res, n,
                             true);
  else if (log2(elementSize) == 2) {
    compute_min_max<int32_t>((int32_t *)a, (int32_t *)b, (int32_t *)res, n,
                             true);
  } else if (log2(elementSize) == 3)
    compute_min_max<int64_t>((int64_t *)a, (int64_t *)b, (int64_t *)res, n,
                             true);
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
    compute_min_max<int8_t>((int8_t *)a, (int8_t *)b, (int8_t *)res, n, false);
  else if (log2(elementSize) == 1)
    compute_min_max<int16_t>((int16_t *)a, (int16_t *)b, (int16_t *)res, n,
                             false);
  else if (log2(elementSize) == 2)
    compute_min_max<int32_t>((int32_t *)a, (int32_t *)b, (int32_t *)res, n,
                             false);
  else if (log2(elementSize) == 3)
    compute_min_max<int64_t>((int64_t *)a, (int64_t *)b, (int64_t *)res, n,
                             false);
}

void send_bool(bool *b, int n) {
  ios[0]->send_bool(b, n);
  ios[0]->flush();
}
void recv_bool(bool *b, int n) { ios[0]->recv_bool(b, n); }

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

template <typename T, typename U>
__attribute__((always_inline)) void compute(T *input1, T *input2, T *output,
                                            int n, U *func) {
  bool *i1 = new bool[sizeof(T) * 8 * 2 * n];
  bool *o = new bool[sizeof(T) * 8 * n];
  for (int i = 0; i < n; ++i) {
    int_to_bool<T>(i1 + i * sizeof(T) * 8, input1[i], sizeof(T) * 8);
    int_to_bool<T>(i1 + n * sizeof(T) * 8 + i * sizeof(T) * 8, input2[i],
                   sizeof(T) * 8);
  }
  func->template compute<NetIO>(o, i1, n, true);
  for (int i = 0; i < n; ++i) {
    output[i] = bool_to_int<T>(o + i * sizeof(T) * 8);
  }
  delete[] i1;
  delete[] o;
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

  if (div[(int)log2(elementSize)] == nullptr) {
    string name = "/usr/local/include/mpc/circuits/div";
    name += std::to_string(elementSize * 8) + ".txt";
    div[(int)log2(elementSize)] =
        new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
  }

  if (log2(elementSize) == 0)
    compute<int8_t, Circuit<SIMDCircExec<NetIO>>>((int8_t *)a, (int8_t *)b,
                                                  (int8_t *)res, n, div[0]);
  else if (log2(elementSize) == 1)
    compute<int16_t, Circuit<SIMDCircExec<NetIO>>>((int16_t *)a, (int16_t *)b,
                                                   (int16_t *)res, n, div[1]);
  else if (log2(elementSize) == 2) {
    compute<int32_t, Circuit<SIMDCircExec<NetIO>>>((int32_t *)a, (int32_t *)b,
                                                   (int32_t *)res, n, div[2]);
  } else if (log2(elementSize) == 3)
    compute<int64_t, Circuit<SIMDCircExec<NetIO>>>((int64_t *)a, (int64_t *)b,
                                                   (int64_t *)res, n, div[3]);
}

template <typename T> void redHelper(T *arr, T *res, int n, int op) {

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
      res[0] |= arr[i];
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
  if (elementSize == 0) {
    bool *result = (bool *)res;
    if (res == falseVal || res == condition) {
      result = new bool[n];
    }
    for (int i = 0; i < n; ++i)
      result[i] = ((bool *)falseVal)[i] ^ ((bool *)trueVal)[i];
    simd_circ->and_gate(result, result, condition, n);

    for (int i = 0; i < n; ++i)
      result[i] ^= ((bool *)falseVal)[i];

    // for (int i = 0; i < n; ++i) {
    //   printf("%d %hd %hd %hd %hd\n", selectNum, ((bool *)condition)[i],
    //          ((bool *)trueVal)[i], ((bool *)falseVal)[i], ((bool
    //          *)result)[i]);
    //   selectNum += 1;
    // }
    if (res == falseVal || res == condition) {
      memcpy(res, result, n);
      delete result;
    }

    return;
  }
  bool *in1 = new bool[n * elementSize * 8];
  bool *in2 = new bool[n * elementSize * 8];
  for (int i = 0; i < n; ++i) {
    if (log2(elementSize) == 0) {
      int_to_bool<int8_t>(in1 + i * elementSize * 8, ((int8_t *)trueVal)[i],
                          elementSize * 8);
      int_to_bool<int8_t>(in2 + i * elementSize * 8, ((int8_t *)falseVal)[i],
                          elementSize * 8);
    } else if (log2(elementSize) == 1) {
      int_to_bool<int16_t>(in1 + i * elementSize * 8, ((int16_t *)trueVal)[i],
                           elementSize * 8);
      int_to_bool<int16_t>(in2 + i * elementSize * 8, ((int16_t *)falseVal)[i],
                           elementSize * 8);
    } else if (log2(elementSize) == 2) {
      int_to_bool<int32_t>(in1 + i * elementSize * 8, ((int32_t *)trueVal)[i],
                           elementSize * 8);
      int_to_bool<int32_t>(in2 + i * elementSize * 8, ((int32_t *)falseVal)[i],
                           elementSize * 8);
    } else if (log2(elementSize) == 3) {
      int_to_bool<int64_t>(in1 + i * elementSize * 8, ((int64_t *)trueVal)[i],
                           elementSize * 8);
      int_to_bool<int64_t>(in2 + i * elementSize * 8, ((int64_t *)falseVal)[i],
                           elementSize * 8);
    }
  }

  for (int i = 0; i < n * elementSize * 8; ++i)
    in1[i] ^= in2[i];

  for (int i = 0; i < n; ++i)
    memset(in2 + i * elementSize * 8, condition[i], elementSize * 8);

  simd_circ->and_gate(in1, in1, in2, n * elementSize * 8);
  for (int i = 0; i < n; ++i) {
    if (log2(elementSize) == 0)
      int_to_bool<int8_t>(in2 + i * elementSize * 8, ((int8_t *)falseVal)[i],
                          elementSize * 8);
    else if (log2(elementSize) == 1)
      int_to_bool<int16_t>(in2 + i * elementSize * 8, ((int16_t *)falseVal)[i],
                           elementSize * 8);
    else if (log2(elementSize) == 2)
      int_to_bool<int32_t>(in2 + i * elementSize * 8, ((int32_t *)falseVal)[i],
                           elementSize * 8);
    else if (log2(elementSize) == 3)
      int_to_bool<int64_t>(in2 + i * elementSize * 8, ((int64_t *)falseVal)[i],
                           elementSize * 8);
  }

  for (int i = 0; i < n * elementSize * 8; ++i)
    in1[i] ^= in2[i];

  for (int i = 0; i < n; ++i) {
    if (log2(elementSize) == 0)
      ((int8_t *)res)[i] = bool_to_int<int8_t>(in1 + i * elementSize * 8);
    else if (log2(elementSize) == 1)
      ((int16_t *)res)[i] = bool_to_int<int16_t>(in1 + i * elementSize * 8);
    else if (log2(elementSize) == 2)
      ((int32_t *)res)[i] = bool_to_int<int32_t>(in1 + i * elementSize * 8);
    else if (log2(elementSize) == 3)
      ((int64_t *)res)[i] = bool_to_int<int64_t>(in1 + i * elementSize * 8);
  }
}

void reduction(void *arr, int n, int elementSize, void *res, int op,
               bool shared) {
  if (!shared) {
    if (log2(elementSize) == 0)
      redHelper<int8_t>((int8_t *)arr, (int8_t *)res, n, op);
    else if (log2(elementSize) == 1)
      redHelper<int16_t>((int16_t *)arr, (int16_t *)res, n, op);
    else if (log2(elementSize) == 2)
      redHelper<int32_t>((int32_t *)arr, (int32_t *)res, n, op);
    else if (log2(elementSize) == 3)
      redHelper<int64_t>((int64_t *)arr, (int64_t *)res, n, op);

    return;
  }
  // 1 => 8, 2 => 16, 4 => 32, 8 =>64

  std::unique_ptr<bool[]> in = std::make_unique<bool[]>(n * elementSize * 8);
  for (int i = 0; i < n; ++i) {
    if (log2(elementSize) == 0)
      int_to_bool<int8_t>(in.get() + i * elementSize * 8, ((int8_t *)arr)[i],
                          elementSize * 8);
    else if (log2(elementSize) == 1)
      int_to_bool<int16_t>(in.get() + i * elementSize * 8, ((int16_t *)arr)[i],
                           elementSize * 8);
    else if (log2(elementSize) == 2)
      int_to_bool<int32_t>(in.get() + i * elementSize * 8, ((int32_t *)arr)[i],
                           elementSize * 8);
    else if (log2(elementSize) == 3)
      int_to_bool<int64_t>(in.get() + i * elementSize * 8, ((int64_t *)arr)[i],
                           elementSize * 8);
  }

  /* smax => bool condition = (a > b), condition &(a^b)^b
  smin => bool condition = (a > b), condition &(a^b)^a
  */
  if (op == reductionOp::OR) {
    for (uint i = 0; i < n * elementSize * 8; ++i)
      in[i] = in[i] ^ 1;
  }

  Circuit<SIMDCircExec<NetIO>> *circ;
  if (op == reductionOp::ADD) {
    if (adder[(int)log2(elementSize)] == nullptr) {
      string name = "/usr/local/include/mpc/circuits/adder";
      name += std::to_string(elementSize * 8) + ".txt";
      adder[(int)log2(elementSize)] =
          new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
    }
    circ = adder[(int)log2(elementSize)];
  } else if (op == reductionOp::MUL) {
    if (mult[(int)log2(elementSize)] == nullptr) {
      string name = "/usr/local/include/mpc/circuits/mult";
      name += std::to_string(elementSize * 8) + ".txt";
      mult[(int)log2(elementSize)] =
          new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
    }
    circ = mult[(int)log2(elementSize)];
  } else if (op == reductionOp::MAX || op == reductionOp::MIN) {
    if (gt[(int)log2(elementSize)] == nullptr) {
      string name = "/usr/local/include/mpc/circuits/gt";
      name += std::to_string(elementSize * 8) + ".txt";
      gt[(int)log2(elementSize)] =
          new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
    }
    circ = gt[(int)log2(elementSize)];
  }

  int l_floor = n;
  while (l_floor != 1) {
    int l = l_floor % 2;
    l_floor = l_floor >> 1;
    l = l + l_floor;
    if (op == reductionOp::MAX || op == reductionOp::MIN) {
      bool *cmp = new bool[l_floor * 8];
      circ->compute<NetIO>(cmp, in.get(), l_floor, true);
      bool *condition = new bool[l_floor * elementSize * 8];
      for (int i = 0; i < l_floor; ++i) {
        int8_t x = bool_to_int<int8_t>(cmp + i * 8);
        memset(condition + (i * elementSize * 8), x == 0 ? false : true,
               elementSize * 8);
      }
      int offset = 0;
      if (op == reductionOp::MIN)
        offset = l_floor * elementSize * 8;
      xorBools_arr(in.get() + offset, in.get(),
                   in.get() + l_floor * elementSize * 8,
                   l_floor * elementSize * 8);
      simd_circ->and_gate(condition, condition, in.get() + offset,
                          l_floor * elementSize * 8);
      xorBools_arr(in.get(), condition,
                   offset == 0 ? in.get() + l_floor * elementSize * 8
                               : in.get(),
                   l_floor * elementSize * 8);
      delete[] cmp;
      delete[] condition;
    } else if (op == reductionOp::OR) {
      simd_circ->and_gate(in.get(), in.get(), in.get() + l_floor, l_floor);
    } else {
      circ->compute<NetIO>(in.get(), in.get(), l_floor, true);
    }
    if (l != l_floor) {
      memcpy(in.get() + l_floor * elementSize * 8,
             in.get() + l_floor * elementSize * 2 * 8, elementSize * 8);
    }
    l_floor = l;
  }
  if (log2(elementSize) == 0) {
    int8_t output = bool_to_int<int8_t>(in.get());
    int8_t *r = (int8_t *)res;
    r[0] = output;
  } else if (log2(elementSize) == 1) {
    int16_t output = bool_to_int<int16_t>(in.get());
    int16_t *r = (int16_t *)res;
    r[0] = output;
  } else if (log2(elementSize) == 2) {
    int32_t output = bool_to_int<int32_t>(in.get());
    int32_t *r = (int32_t *)res;
    r[0] = output;
  } else if (log2(elementSize) == 3) {
    int64_t output = bool_to_int<int64_t>(in.get());
    int64_t *r = (int64_t *)res;
    r[0] = output;
  }
  if (simd_circ->depth == prev_depth) {
    std::cout << "reduction\n";
  }
  prev_depth = simd_circ->depth;
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
      dataRecv[i] ^= data[i];
    }
    return dataRecv;
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
      dataRecv[i] ^= data[i];
    }
    return dataRecv;
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
template uint64_t *reveal<uint64_t>(uint64_t *data, int num, int p);

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
  int threads = 4;
  for (int i = 0; i < threads; ++i)
    ios.push_back(
        new NetIO(party == ALICE ? nullptr : "127.0.0.1", port, true));

  auto start = clock_start();
  simd_circ = new emp::SIMDCircExec<NetIO>(party, threads, ios.data());
  auto time = time_from(start);
  std::cout << "simd circ exec " << time / 1000000 << " "
            << simd_circ->num_block_triples_pool * 128 << " \n";

  // adder[0] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/adder8.txt", party, simd_circ);
  // adder[1] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/adder16.txt", party, simd_circ);
  // adder[2] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/adder32.txt", party, simd_circ);
  // adder[3] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/adder64.txt", party, simd_circ);

  // mult[0] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/mult8.txt", party, simd_circ);
  // mult[1] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/mult16.txt", party, simd_circ);
  // mult[2] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/mult32.txt", party, simd_circ);
  // mult[3] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/mult64.txt", party, simd_circ);

  // eq[0] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/icmpeq8.txt", party, simd_circ);
  // eq[1] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/icmpeq16.txt", party, simd_circ);
  // eq[2] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/icmpeq32.txt", party, simd_circ);
  // eq[3] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/icmpeq64.txt", party, simd_circ);

  // gt[0] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/gt8.txt", party, simd_circ);
  // gt[1] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/gt16.txt", party, simd_circ);
  // gt[2] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/gt32.txt", party, simd_circ);
  // gt[3] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/gt64.txt", party, simd_circ);

  // ge[0] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/ge8.txt", party, simd_circ);
  // ge[1] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/ge16.txt", party, simd_circ);
  // ge[2] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/ge32.txt", party, simd_circ);
  // ge[3] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/ge64.txt", party, simd_circ);

  // sub[0] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/sub8.txt", party, simd_circ);
  // sub[1] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/sub16.txt", party, simd_circ);
  // sub[2] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/sub32.txt", party, simd_circ);
  // sub[3] = new Circuit<SIMDCircExec<NetIO>>(
  //     "/usr/local/include/mpc/circuits/sub64.txt", party, simd_circ);
}
template <typename T>
void loadStoreHelper(bool *vals, int32_t idx, int32_t n, bool *andOutput) {
  // compare the indices with wanted index, results in 8 bit output for n
  // and with the values boolean (n * sizeof(T) * 8)
  bool *input = new bool[n * 2 * 32];
  memset(input, 0, n * 2 * 32);
  for (int i = 0; i < n; ++i) {
    if (party == 1)
      int_to_bool<int32_t>(input + 32 * i, i, 32);
    int_to_bool<int32_t>(input + n * 32 + 32 * i, idx, 32);
  }
  bool *cmp = new bool[n];
  if (!eq[2])
    eq[2] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/icmpeq32.txt", party, simd_circ);

  eq[2]->compute<NetIO>(cmp, input, n, true);

  delete[] input;
  // loaded values, and cmp, xor
  for (int i = 0; i < n; ++i) {
    memset(andOutput + i * sizeof(T) * 8, cmp[i], sizeof(T) * 8);
  }

  simd_circ->and_gate(andOutput, vals, andOutput, n * sizeof(T) * 8);
  delete[] cmp;
}

void load(void *arr, int32_t idx, void *res, int n, int elementSize,
          bool isPublic = false) {
  bool *andOutput = new bool[n * elementSize * 8];
  bool *in = new bool[n * elementSize * 8];
  if (isPublic && party == 1)
    memset(in, 0, n * elementSize * 8);
  else {
    for (int i = 0; i < n; ++i) {
      if (log2(elementSize) == 0)
        int_to_bool<int8_t>(in + i * elementSize * 8, ((int8_t *)arr)[i],
                            elementSize * 8);
      else if (log2(elementSize) == 1)
        int_to_bool<int16_t>(in + i * elementSize * 8, ((int16_t *)arr)[i],
                             elementSize * 8);
      else if (log2(elementSize) == 2)
        int_to_bool<int32_t>(in + i * elementSize * 8, ((int32_t *)arr)[i],
                             elementSize * 8);
      else if (log2(elementSize) == 3)
        int_to_bool<int64_t>(in + i * elementSize * 8, ((int64_t *)arr)[i],
                             elementSize * 8);
    }
  }
  if (log2(elementSize) == 0)
    loadStoreHelper<int8_t>(in, idx, n, andOutput);
  else if (log2(elementSize) == 1)
    loadStoreHelper<int16_t>(in, idx, n, andOutput);
  else if (log2(elementSize) == 2)
    loadStoreHelper<int32_t>(in, idx, n, andOutput);
  else if (log2(elementSize) == 3)
    loadStoreHelper<int64_t>(in, idx, n, andOutput);
  delete[] in;
  bool *output = new bool[8 * elementSize];
  memset(output, 0, elementSize * 8);
  for (int i = 0; i < n; ++i) {
    // int32_t x = bool_to_int<int32_t>(andOutput + i * 8 * elementSize);
    // printf("%d %d %d %d\n", party, idx, i, x);
    xorBools_arr(output, output, andOutput + i * 8 * elementSize,
                 8 * elementSize);
  }

  if (log2(elementSize) == 0)
    ((int8_t *)res)[0] = bool_to_int<int8_t>(output);
  else if (log2(elementSize) == 1)
    ((int16_t *)res)[0] = bool_to_int<int16_t>(output);
  else if (log2(elementSize) == 2)
    ((int32_t *)res)[0] = bool_to_int<int32_t>(output);
  else if (log2(elementSize) == 3)
    ((int64_t *)res)[0] = bool_to_int<int64_t>(output);
  delete[] output;
  delete[] andOutput;
}

void store(void *arr, void *val, int32_t idx, int32_t n, int32_t elementSize,
           bool isPublic) {

  bool *input = new bool[n * elementSize * 8 * 2];
  bool *vals = new bool[n * elementSize * 8];
  memset(input, 0, n * elementSize * 8 * 2);

  for (int i = 0; i < n; ++i) {
    if (log2(elementSize) == 0) {
      int_to_bool<int8_t>(input + elementSize * 8 * (n + i), ((int8_t *)arr)[i],
                          elementSize * 8);
      int_to_bool<int8_t>(input + elementSize * 8 * i,
                          (isPublic && party == 1) ? 0 : ((int8_t *)val)[0],
                          8 * elementSize);
    } else if (log2(elementSize) == 1) {
      int_to_bool<int16_t>(input + elementSize * 8 * (n + i),
                           ((int16_t *)arr)[i], elementSize * 8);
      int_to_bool<int16_t>(input + elementSize * 8 * i,
                           (isPublic && party == 1) ? 0 : ((int16_t *)val)[0],
                           8 * elementSize);
    } else if (log2(elementSize) == 2) {
      int_to_bool<int32_t>(input + elementSize * 8 * (n + i),
                           ((int32_t *)arr)[i], elementSize * 8);
      int_to_bool<int32_t>(input + elementSize * 8 * i,
                           (isPublic && party == 1) ? 0 : ((int32_t *)val)[0],
                           8 * elementSize);
    } else if (log2(elementSize) == 3) {
      int_to_bool<int64_t>(input + elementSize * 8 * (n + i),
                           ((int64_t *)arr)[i], elementSize * 8);
      int_to_bool<int64_t>(input + elementSize * 8 * i,
                           (isPublic && party == 1) ? 0 : ((int64_t *)val)[0],
                           8 * elementSize);
    }
  }

  if (sub[(int)log2(elementSize)] == nullptr) {
    string name = "/usr/local/include/mpc/circuits/sub";
    name += std::to_string(elementSize * 8) + ".txt";
    sub[(int)log2(elementSize)] =
        new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
  }
  sub[(int)log2(elementSize)]->compute<NetIO>(vals, input, n, true);

  if (log2(elementSize) == 0)
    loadStoreHelper<int8_t>(vals, idx, n, input);
  else if (log2(elementSize) == 1)
    loadStoreHelper<int16_t>(vals, idx, n, input);
  else if (log2(elementSize) == 2)
    loadStoreHelper<int32_t>(vals, idx, n, input);
  else if (log2(elementSize) == 3)
    loadStoreHelper<int64_t>(vals, idx, n, input);

  for (int i = 0; i < n; ++i)
    if (log2(elementSize) == 0) {
      int_to_bool<int8_t>(input + elementSize * 8 * (n + i), ((int8_t *)arr)[i],
                          elementSize * 8);
    } else if (log2(elementSize) == 1) {
      int_to_bool<int16_t>(input + elementSize * 8 * (n + i),
                           ((int16_t *)arr)[i], elementSize * 8);
    } else if (log2(elementSize) == 2) {
      int_to_bool<int32_t>(input + elementSize * 8 * (n + i),
                           ((int32_t *)arr)[i], elementSize * 8);
    } else if (log2(elementSize) == 3) {
      int_to_bool<int64_t>(input + elementSize * 8 * (n + i),
                           ((int64_t *)arr)[i], elementSize * 8);
    }

  memset(vals, 0, n * elementSize * 8);
  if (adder[(int)log2(elementSize)] == nullptr) {
    string name = "/usr/local/include/mpc/circuits/adder";
    name += std::to_string(elementSize * 8) + ".txt";
    adder[(int)log2(elementSize)] =
        new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
  }
  adder[(int)log2(elementSize)]->compute<NetIO>(vals, input, n, true);

  for (int i = 0; i < n; ++i)
    if (log2(elementSize) == 0)
      ((int8_t *)arr)[i] = bool_to_int<int8_t>(vals + i * elementSize * 8);
    else if (log2(elementSize) == 1)
      ((int16_t *)arr)[i] = bool_to_int<int16_t>(vals + i * elementSize * 8);
    else if (log2(elementSize) == 2)
      ((int32_t *)arr)[i] = bool_to_int<int32_t>(vals + i * elementSize * 8);
    else if (log2(elementSize) == 3)
      ((int64_t *)arr)[i] = bool_to_int<int64_t>(vals + i * elementSize * 8);

  delete[] vals;
  delete[] input;
}

void storeConst(void *a, int8_t *b, int32_t n, int m) {
  int8_t *tmp = (int8_t *)a;
  for (int i = 0; i < n / m; ++i) {
    for (int j = 0; j < m; ++j) {
      tmp[i * m + j] = b[j];
    }
  }
}

template <typename T> void compute(T *input1, T *input2, T *output, int n) {
  bool *i1 = new bool[sizeof(T) * 8 * n];
  bool *i2 = new bool[sizeof(T) * 8 * n];
  bool *o = new bool[sizeof(T) * 8 * n];
  for (int i = 0; i < n; ++i) {
    int_to_bool<T>(i1 + i * sizeof(T) * 8, input1[i], sizeof(T) * 8);
    int_to_bool<T>(i2 + i * sizeof(T) * 8, input2[i], sizeof(T) * 8);
  }
  simd_circ->and_gate(o, i1, i2, n * sizeof(T) * 8);

  for (int i = 0; i < n; ++i) {
    output[i] = bool_to_int<T>(o + i * sizeof(T) * 8);
  }
  delete[] i1;
  delete[] i2;
  delete[] o;
}

void andBool(bool *input1, bool *input2, bool *output, int n,
             bool bothPrivate) {
  if (!bothPrivate) {
    for (int i = 0; i < n; ++i) {
      output[i] = input1[i] & input2[i];
    }
  } else
    simd_circ->and_gate(output, input1, input2, n);
}
void andI8(int8_t *input1, int8_t *input2, int8_t *output, int n,
           bool bothPrivate) {
  if (!bothPrivate) {
    for (int i = 0; i < n; ++i) {
      output[i] = input1[i] & input2[i];
    }
  } else
    compute<int8_t>(input1, input2, output, n);
}
void andI16(int16_t *input1, int16_t *input2, int16_t *output, int n,
            bool bothPrivate) {
  if (!bothPrivate) {
    for (int i = 0; i < n; ++i) {
      output[i] = input1[i] & input2[i];
    }
  } else
    compute<int16_t>(input1, input2, output, n);
}
void andI32(int32_t *input1, int32_t *input2, int32_t *output, int n,
            bool bothPrivate) {
  if (!bothPrivate) {
    for (int i = 0; i < n; ++i) {
      output[i] = input1[i] & input2[i];
    }
  } else
    compute<int32_t>(input1, input2, output, n);
}
void andI64(int64_t *input1, int64_t *input2, int64_t *output, int n,
            bool bothPrivate) {
  if (!bothPrivate) {
    for (int i = 0; i < n; ++i) {
      output[i] = input1[i] & input2[i];
    }
  } else
    compute<int64_t>(input1, input2, output, n);
}

void xorBool(bool *input1, bool *input2, bool *output, int n, bool bothSame) {
  if (party == 2 && !bothSame)
    return;
  for (int i = 0; i < n; ++i)
    output[i] = input1[i] ^ input2[i];
}
void xorI8(int8_t *input1, int8_t *input2, int8_t *output, int n,
           bool bothSame) {
  if (party == 2 && !bothSame)
    return;

  for (int i = 0; i < n; ++i)
    output[i] = input1[i] ^ input2[i];
}
void xorI16(int16_t *input1, int16_t *input2, int16_t *output, int n,
            bool bothSame) {
  if (party == 2 && !bothSame)
    return;

  for (int i = 0; i < n; ++i)
    output[i] = input1[i] ^ input2[i];
}
void xorI32(int32_t *input1, int32_t *input2, int32_t *output, int n,
            bool bothSame) {
  if (party == 2 && !bothSame) {
    std::cout << "both not same\n";
    return;
  }

  for (int i = 0; i < n; ++i)
    output[i] = input1[i] ^ input2[i];
}
void xorI64(int64_t *input1, int64_t *input2, int64_t *output, int n,
            bool bothSame) {
  if (party == 2 && !bothSame)
    return;

  for (int i = 0; i < n; ++i)
    output[i] = input1[i] ^ input2[i];
}

void addI8(int8_t *input1, int8_t *input2, int8_t *output, int n) {
  if (!adder[0])
    adder[0] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/adder8.txt", party, simd_circ);
  compute<int8_t, Circuit<SIMDCircExec<NetIO>>>(input1, input2, output, n,
                                                adder[0]);
}
void addI16(int16_t *input1, int16_t *input2, int16_t *output, int n) {
  if (!adder[1])
    adder[1] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/adder16.txt", party, simd_circ);
  compute<int16_t, Circuit<SIMDCircExec<NetIO>>>(input1, input2, output, n,
                                                 adder[1]);
}
void addI32(int32_t *input1, int32_t *input2, int32_t *output, int n) {
  if (!adder[2])
    adder[2] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/adder32.txt", party, simd_circ);
  compute<int32_t, Circuit<SIMDCircExec<NetIO>>>(input1, input2, output, n,
                                                 adder[2]);
}
void addI64(int64_t *input1, int64_t *input2, int64_t *output, int n) {
  if (!adder[3])
    adder[3] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/adder64.txt", party, simd_circ);
  compute<int64_t, Circuit<SIMDCircExec<NetIO>>>(input1, input2, output, n,
                                                 adder[3]);
}
void subI8(int8_t *input1, int8_t *input2, int8_t *output, int n) {
  if (!sub[0])
    sub[0] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/sub8.txt", party, simd_circ);
  compute<int8_t, Circuit<SIMDCircExec<NetIO>>>(input1, input2, output, n,
                                                sub[0]);
}
void subI16(int16_t *input1, int16_t *input2, int16_t *output, int n) {
  if (!sub[1])
    sub[1] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/sub16.txt", party, simd_circ);
  compute<int16_t, Circuit<SIMDCircExec<NetIO>>>(input1, input2, output, n,
                                                 sub[1]);
}

void subI32(int32_t *input1, int32_t *input2, int32_t *output, int n) {
  if (!sub[2])
    sub[2] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/sub32.txt", party, simd_circ);
  compute<int32_t, Circuit<SIMDCircExec<NetIO>>>(input1, input2, output, n,
                                                 sub[2]);

  if (simd_circ->depth == prev_depth) {
    std::cout << "subi32" << prev_depth << " " << simd_circ->depth << "\n";
  }
  prev_depth = simd_circ->depth;
}
void subI64(int64_t *input1, int64_t *input2, int64_t *output, int n) {
  if (!sub[3])
    sub[3] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/sub64.txt", party, simd_circ);
  compute<int64_t, Circuit<SIMDCircExec<NetIO>>>(input1, input2, output, n,
                                                 sub[3]);
}
void multI8(int8_t *input1, int8_t *input2, int8_t *output, int n) {
  if (!mult[0])
    mult[0] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/mult8.txt", party, simd_circ);
  compute<int8_t, Circuit<SIMDCircExec<NetIO>>>(input1, input2, output, n,
                                                mult[0]);
}
void multI16(int16_t *input1, int16_t *input2, int16_t *output, int n) {
  if (!mult[1])
    mult[1] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/mult16.txt", party, simd_circ);
  compute<int16_t, Circuit<SIMDCircExec<NetIO>>>(input1, input2, output, n,
                                                 mult[1]);
}

void multI32(int32_t *input1, int32_t *input2, int32_t *output, int n) {
  if (!mult[2])
    mult[2] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/mult32.txt", party, simd_circ);
  compute<int32_t, Circuit<SIMDCircExec<NetIO>>>(input1, input2, output, n,
                                                 mult[2]);
  if (simd_circ->depth == prev_depth) {
    std::cout << "multI32\n";
  }
  prev_depth = simd_circ->depth;
}
void multI64(int64_t *input1, int64_t *input2, int64_t *output, int n) {
  if (!mult[3])
    mult[3] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/mult64.txt", party, simd_circ);
  compute<int64_t, Circuit<SIMDCircExec<NetIO>>>(input1, input2, output, n,
                                                 mult[3]);
}

template <typename T>
__attribute__((always_inline)) void computeIcmp(T *input1, T *input2,
                                                bool *output, int op, int n) {
  bool *i1 = new bool[sizeof(T) * 8 * 2 * n];
  for (int i = 0; i < n; ++i) {
    int_to_bool<T>(i1 + i * sizeof(T) * 8, input1[i], sizeof(T) * 8);
    int_to_bool<T>(i1 + n * sizeof(T) * 8 + i * sizeof(T) * 8, input2[i],
                   sizeof(T) * 8);
  }
  switch (op) {
  case cmp::EQ:
  case cmp::NE:
    if (eq[(int)log2(sizeof(T))] == nullptr) {
      string name = "/usr/local/include/mpc/circuits/icmpeq";
      name += std::to_string(sizeof(T) * 8) + ".txt";
      eq[(int)log2(sizeof(T))] =
          new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
    }
    break;
  case cmp::GT:
    if (gt[(int)log2(sizeof(T))] == nullptr) {
      string name = "/usr/local/include/mpc/circuits/gt";
      name += std::to_string(sizeof(T) * 8) + ".txt";
      gt[(int)log2(sizeof(T))] =
          new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
    }
    break;
  case cmp::GE:
    if (ge[(int)log2(sizeof(T))] == nullptr) {
      string name = "/usr/local/include/mpc/circuits/ge";
      name += std::to_string(sizeof(T) * 8) + ".txt";
      ge[(int)log2(sizeof(T))] =
          new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
    }
    break;
  default:
    break;
  }
  switch (op) {
  case cmp::EQ:
    eq[(int)log2(sizeof(T))]->compute<NetIO>(output, i1, n, true);
    break;
  case cmp::GT:
    gt[(int)log2(sizeof(T))]->compute<NetIO>(output, i1, n, true);
    break;
  case cmp::GE:
    ge[(int)log2(sizeof(T))]->compute<NetIO>(output, i1, n, true);
    break;
  case cmp::NE:
    eq[(int)log2(sizeof(T))]->compute<NetIO>(output, i1, n, true);
    break;
  default:
    break;
  }
  // for (int i = 0; i < n; ++i) {
  //   int8_t x = bool_to_int<int8_t>(o + i * 8);
  //   if (x == 0)
  //     output[i] = false;
  //   else
  //     output[i] = true;
  // }
  if (op == cmp::NE) {
    simd_circ->not_gate(output, output, n);
  }
  delete[] i1;
}

void icmpEqI8(int8_t *input1, int8_t *input2, bool *output, int n, int op) {
  computeIcmp<int8_t>(input1, input2, output, op, n);
}
void icmpEqI16(int16_t *input1, int16_t *input2, bool *output, int n, int op) {
  computeIcmp<int16_t>(input1, input2, output, op, n);
}
void icmpEqI32(int32_t *input1, int32_t *input2, bool *output, int n, int op) {
  computeIcmp<int32_t>(input1, input2, output, op, n);
  if (simd_circ->depth == prev_depth) {
    std::cout << "subi32\n";
  }
  prev_depth = simd_circ->depth;
}
void icmpEqI64(int64_t *input1, int64_t *input2, bool *output, int n, int op) {
  computeIcmp<int64_t>(input1, input2, output, op, n);
}

bool andBool(bool input1, bool input2) {
  bool *i1 = new bool[1];
  bool *i2 = new bool[1];
  bool *o = new bool[1];
  i1[0] = input1;
  i2[0] = input2;
  simd_circ->and_gate(o, i1, i2, 1);
  bool output = o[0];
  delete[] i1;
  delete[] i2;
  delete[] o;
  return output;
}
int32_t andI32(int32_t input1, int32_t input2) {
  bool *i1 = new bool[32];
  bool *i2 = new bool[32];
  bool *o = new bool[32];
  int_to_bool<int32_t>(i1, input1, 32);
  int_to_bool<int32_t>(i2, input2, 32);
  simd_circ->and_gate(o, i1, i2, 32);
  int32_t output = bool_to_int<int32_t>(o);
  delete[] i1;
  delete[] i2;
  delete[] o;
  if (simd_circ->depth == prev_depth) {
    std::cout << "andi32\n";
  }
  prev_depth = simd_circ->depth;
  return output;
}
int64_t andI64(int64_t input1, int64_t input2) {
  bool *i1 = new bool[64];
  bool *i2 = new bool[64];
  bool *o = new bool[64];
  int_to_bool<int64_t>(i1, input1, 64);
  int_to_bool<int64_t>(i2, input2, 64);
  simd_circ->and_gate(o, i1, i2, 64);
  int64_t output = bool_to_int<int64_t>(o);
  delete[] i1;
  delete[] i2;
  delete[] o;
  return output;
}
int16_t andI16(int16_t input1, int16_t input2) {
  bool *i1 = new bool[16];
  bool *i2 = new bool[16];
  bool *o = new bool[16];
  int_to_bool<int16_t>(i1, input1, 16);
  int_to_bool<int16_t>(i2, input2, 16);
  simd_circ->and_gate(o, i1, i2, 16);
  int16_t output = bool_to_int<int16_t>(o);
  delete[] i1;
  delete[] i2;
  delete[] o;
  return output;
}
int8_t andI8(int8_t input1, int8_t input2) {
  bool *i1 = new bool[8];
  bool *i2 = new bool[8];
  bool *o = new bool[8];
  int_to_bool<int8_t>(i1, input1, 8);
  int_to_bool<int8_t>(i2, input2, 8);
  simd_circ->and_gate(o, i1, i2, 8);
  int8_t output = bool_to_int<int8_t>(o);
  delete[] i1;
  delete[] i2;
  delete[] o;
  return output;
}
int8_t addI8(int8_t input1, int8_t input2) {
  int8_t output;
  if (!adder[0])
    adder[0] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/adder8.txt", party, simd_circ);
  compute<int8_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                adder[0]);
  return output;
}

int16_t addI16(int16_t input1, int16_t input2) {
  int16_t output;
  if (!adder[1])
    adder[1] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/adder16.txt", party, simd_circ);
  compute<int16_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                 adder[1]);
  return output;
}

int32_t addI32(int32_t input1, int32_t input2) {
  int32_t output;
  if (!adder[2])
    adder[2] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/adder32.txt", party, simd_circ);
  compute<int32_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                 adder[2]);
  return output;
}
int64_t addI64(int64_t input1, int64_t input2) {
  int64_t output;
  if (!adder[3])
    adder[3] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/adder64.txt", party, simd_circ);
  compute<int64_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                 adder[3]);
  return output;
}

int8_t subI8(int8_t input1, int8_t input2) {
  int8_t output;
  if (!sub[0])
    sub[0] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/sub8.txt", party, simd_circ);
  compute<int8_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                sub[0]);
  return output;
}

int16_t subI16(int16_t input1, int16_t input2) {
  int16_t output;
  if (!sub[1])
    sub[1] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/sub16.txt", party, simd_circ);
  compute<int16_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                 sub[1]);
  return output;
}

int32_t subI32(int32_t input1, int32_t input2) {
  int32_t output;
  if (!sub[2])
    sub[2] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/sub32.txt", party, simd_circ);
  compute<int32_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                 sub[2]);
  return output;
}
int64_t subI64(int64_t input1, int64_t input2) {
  int64_t output;
  if (!sub[3])
    sub[3] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/sub64.txt", party, simd_circ);
  compute<int64_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                 sub[3]);
  return output;
}

int8_t multI8(int8_t input1, int8_t input2) {
  int8_t output;
  if (!mult[0])
    mult[0] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/mult8.txt", party, simd_circ);
  compute<int8_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                mult[0]);
  return output;
}

int16_t multI16(int16_t input1, int16_t input2) {
  int16_t output;
  if (!mult[1])
    mult[1] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/mult16.txt", party, simd_circ);
  compute<int16_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                 mult[1]);
  return output;
}

int32_t multI32(int32_t input1, int32_t input2) {
  int32_t output;
  if (!mult[2])
    mult[2] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/mult32.txt", party, simd_circ);
  compute<int32_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                 mult[2]);
  return output;
}
int64_t multI64(int64_t input1, int64_t input2) {
  int64_t output;
  if (!mult[3])
    mult[3] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/mult64.txt", party, simd_circ);
  compute<int64_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                 mult[3]);
  return output;
}

int8_t divI8(int8_t input1, int8_t input2) {
  int8_t output;
  if (!div[0])
    div[0] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/div8.txt", party, simd_circ);
  compute<int8_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                div[0]);
  return output;
}

int16_t divI16(int16_t input1, int16_t input2) {
  int16_t output;
  if (!div[1])
    div[1] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/div16.txt", party, simd_circ);
  compute<int16_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                 div[1]);
  return output;
}

int32_t divI32(int32_t input1, int32_t input2) {
  int32_t output;
  if (!div[2])
    div[2] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/div32.txt", party, simd_circ);
  compute<int32_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                 div[2]);
  return output;
}
int64_t divI64(int64_t input1, int64_t input2) {
  int64_t output;
  if (!div[3])
    div[3] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/div64.txt", party, simd_circ);
  compute<int64_t, Circuit<SIMDCircExec<NetIO>>>(&input1, &input2, &output, 1,
                                                 div[3]);
  return output;
}

bool icmpEqI8(int8_t input1, int8_t input2, int op) {
  bool output;
  icmpEqI8(&input1, &input2, &output, 1, op);
  return output;
}
bool icmpEqI32(int32_t input1, int32_t input2, int op) {
  bool output;
  icmpEqI32(&input1, &input2, &output, 1, op);
  return output;
}
bool icmpEqI64(int64_t input1, int64_t input2, int op) {
  bool output;
  icmpEqI64(&input1, &input2, &output, 1, op);
  return output;
}
bool icmpEqI16(int16_t input1, int16_t input2, int op) {
  bool output;
  icmpEqI16(&input1, &input2, &output, 1, op);
  return output;
}

void divF(float *input1, float *input2, float *output, int n) {
  uint32_t *i1 = reinterpret_cast<uint32_t *>(input1);
  uint32_t *i2 = reinterpret_cast<uint32_t *>(input2);
  uint32_t *o = reinterpret_cast<uint32_t *>(output);
  if (!div[4])
    div[4] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/divf.txt", party, simd_circ);
  compute<uint32_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, n, div[4]);
}
void addF(float *input1, float *input2, float *output, int n) {

  uint32_t *i1 = reinterpret_cast<uint32_t *>(input1);
  uint32_t *i2 = reinterpret_cast<uint32_t *>(input2);
  uint32_t *o = reinterpret_cast<uint32_t *>(output);
  if (!adder[4])
    adder[4] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/adderf.txt", party, simd_circ);
  compute<uint32_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, n, adder[4]);
}
void subF(float *input1, float *input2, float *output, int n) {

  uint32_t *i1 = reinterpret_cast<uint32_t *>(input1);
  uint32_t *i2 = reinterpret_cast<uint32_t *>(input2);
  uint32_t *o = reinterpret_cast<uint32_t *>(output);
  if (!sub[4])
    sub[4] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/subf.txt", party, simd_circ);
  compute<uint32_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, n, sub[4]);
}
void multF(float *input1, float *input2, float *output, int n) {

  uint32_t *i1 = reinterpret_cast<uint32_t *>(input1);
  uint32_t *i2 = reinterpret_cast<uint32_t *>(input2);
  uint32_t *o = reinterpret_cast<uint32_t *>(output);
  if (!mult[4])
    mult[4] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/multf.txt", party, simd_circ);
  compute<uint32_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, n, mult[4]);
}
void fcmpEq(float *input1, float *input2, bool *output, int n, int op) {
  uint32_t *i1 = reinterpret_cast<uint32_t *>(input1);
  uint32_t *i2 = reinterpret_cast<uint32_t *>(input2);

  bool *boolInput = new bool[sizeof(uint32_t) * 8 * 2 * n];
  for (int i = 0; i < n; ++i) {
    int_to_bool<uint32_t>(boolInput + i * sizeof(uint32_t) * 8, i1[i],
                          sizeof(uint32_t) * 8);
    int_to_bool<uint32_t>(boolInput + n * sizeof(uint32_t) * 8 +
                              i * sizeof(uint32_t) * 8,
                          i2[i], sizeof(uint32_t) * 8);
  }

  auto circ = eq[4];
  switch (op) {
  case cmp::EQ:
  case cmp::NE:
    if (eq[4] == nullptr) {
      string name = "/usr/local/include/mpc/circuits/icmpeqf.txt";
      eq[4] = new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
    }
    circ = eq[4];
    break;
  case cmp::GT:
    if (gt[4] == nullptr) {
      string name = "/usr/local/include/mpc/circuits/gtf.txt";
      gt[4] = new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
    }
    circ = gt[4];
    break;
  case cmp::GE:
    if (ge[4] == nullptr) {
      string name = "/usr/local/include/mpc/circuits/gef.txt";
      ge[4] = new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
    }
    circ = ge[4];
    break;
  default:
    break;
  }

  circ->compute<NetIO>(output, boolInput, n, true);
  if (op == cmp::NE) {
    simd_circ->not_gate(output, output, n);
  }

  delete[] boolInput;
}
float addF(float input1, float input2) {
  float output;
  uint32_t *i1 = reinterpret_cast<uint32_t *>(&input1);
  uint32_t *i2 = reinterpret_cast<uint32_t *>(&input2);
  uint32_t *o = reinterpret_cast<uint32_t *>(&output);
  if (!adder[4])
    adder[4] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/adderf.txt", party, simd_circ);
  compute<uint32_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, 1, adder[4]);
  return output;
}
float subF(float input1, float input2) {
  float output;
  uint32_t *i1 = reinterpret_cast<uint32_t *>(&input1);
  uint32_t *i2 = reinterpret_cast<uint32_t *>(&input2);
  uint32_t *o = reinterpret_cast<uint32_t *>(&output);
  if (!sub[4])
    sub[4] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/subf.txt", party, simd_circ);
  compute<uint32_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, 1, sub[4]);
  return output;
}
float multF(float input1, float input2) {
  float output;
  uint32_t *i1 = reinterpret_cast<uint32_t *>(&input1);
  uint32_t *i2 = reinterpret_cast<uint32_t *>(&input2);
  uint32_t *o = reinterpret_cast<uint32_t *>(&output);
  if (!mult[4])
    mult[4] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/multf.txt", party, simd_circ);
  compute<uint32_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, 1, mult[4]);
  return output;
}
float divF(float input1, float input2) {
  float output;
  uint32_t *i1 = reinterpret_cast<uint32_t *>(&input1);
  uint32_t *i2 = reinterpret_cast<uint32_t *>(&input2);
  uint32_t *o = reinterpret_cast<uint32_t *>(&output);
  if (!div[4])
    div[4] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/divf.txt", party, simd_circ);
  compute<uint32_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, 1, div[4]);
  return output;
}
bool fcmpEq(float input1, float input2, int op) {
  bool output;
  fcmpEq(&input1, &input2, &output, 1, op);
  return output;
}

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

void divF(double *input1, double *input2, double *output, int n) {
  uint64_t *i1 = reinterpret_cast<uint64_t *>(input1);
  uint64_t *i2 = reinterpret_cast<uint64_t *>(input2);
  uint64_t *o = reinterpret_cast<uint64_t *>(output);
  if (!div[5])
    div[5] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/divd.txt", party, simd_circ);
  compute<uint64_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, n, div[5]);
}
void addF(double *input1, double *input2, double *output, int n) {

  uint64_t *i1 = reinterpret_cast<uint64_t *>(input1);
  uint64_t *i2 = reinterpret_cast<uint64_t *>(input2);
  uint64_t *o = reinterpret_cast<uint64_t *>(output);
  if (!adder[5])
    adder[5] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/adderd.txt", party, simd_circ);
  compute<uint64_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, n, adder[5]);
}
void subF(double *input1, double *input2, double *output, int n) {

  uint64_t *i1 = reinterpret_cast<uint64_t *>(input1);
  uint64_t *i2 = reinterpret_cast<uint64_t *>(input2);
  uint64_t *o = reinterpret_cast<uint64_t *>(output);
  if (!sub[5])
    sub[5] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/subd.txt", party, simd_circ);
  compute<uint64_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, n, sub[5]);
}
void multF(double *input1, double *input2, double *output, int n) {

  uint64_t *i1 = reinterpret_cast<uint64_t *>(input1);
  uint64_t *i2 = reinterpret_cast<uint64_t *>(input2);
  uint64_t *o = reinterpret_cast<uint64_t *>(output);
  if (!mult[5])
    mult[5] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/multd.txt", party, simd_circ);
  compute<uint64_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, n, mult[5]);
}
void fcmpEq(double *input1, double *input2, bool *output, int n, int op) {
  uint64_t *i1 = reinterpret_cast<uint64_t *>(input1);
  uint64_t *i2 = reinterpret_cast<uint64_t *>(input2);

  bool *boolInput = new bool[sizeof(uint64_t) * 8 * 2 * n];
  for (int i = 0; i < n; ++i) {
    int_to_bool<uint64_t>(boolInput + i * sizeof(uint64_t) * 8, i1[i],
                          sizeof(uint64_t) * 8);
    int_to_bool<uint64_t>(boolInput + n * sizeof(uint64_t) * 8 +
                              i * sizeof(uint64_t) * 8,
                          i2[i], sizeof(uint64_t) * 8);
  }

  auto circ = eq[5];
  switch (op) {
  case cmp::EQ:
  case cmp::NE:
    if (eq[5] == nullptr) {
      string name = "/usr/local/include/mpc/circuits/icmpeqd.txt";
      eq[5] = new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
    }
    circ = eq[5];
    break;
  case cmp::GT:
    if (gt[5] == nullptr) {
      string name = "/usr/local/include/mpc/circuits/gtd.txt";
      gt[5] = new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
    }
    circ = gt[5];
    break;
  case cmp::GE:
    if (ge[5] == nullptr) {
      string name = "/usr/local/include/mpc/circuits/ged.txt";
      ge[5] = new Circuit<SIMDCircExec<NetIO>>(name.c_str(), party, simd_circ);
    }
    circ = ge[5];
    break;
  default:
    break;
  }

  circ->compute<NetIO>(output, boolInput, n, true);
  if (op == cmp::NE) {
    simd_circ->not_gate(output, output, n);
  }

  delete[] boolInput;
}
double addF(double input1, double input2) {
  double output;
  uint64_t *i1 = reinterpret_cast<uint64_t *>(&input1);
  uint64_t *i2 = reinterpret_cast<uint64_t *>(&input2);
  uint64_t *o = reinterpret_cast<uint64_t *>(&output);
  if (!adder[5])
    adder[5] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/adderd.txt", party, simd_circ);
  compute<uint64_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, 1, adder[5]);
  return output;
}
double subF(double input1, double input2) {
  double output;
  uint64_t *i1 = reinterpret_cast<uint64_t *>(&input1);
  uint64_t *i2 = reinterpret_cast<uint64_t *>(&input2);
  uint64_t *o = reinterpret_cast<uint64_t *>(&output);
  if (!sub[5])
    sub[5] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/subd.txt", party, simd_circ);
  compute<uint64_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, 1, sub[5]);
  return output;
}
double multF(double input1, double input2) {
  double output;
  uint64_t *i1 = reinterpret_cast<uint64_t *>(&input1);
  uint64_t *i2 = reinterpret_cast<uint64_t *>(&input2);
  uint64_t *o = reinterpret_cast<uint64_t *>(&output);
  if (!mult[5])
    mult[5] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/multd.txt", party, simd_circ);
  compute<uint64_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, 1, mult[5]);
  return output;
}
double divF(double input1, double input2) {
  double output;
  uint64_t *i1 = reinterpret_cast<uint64_t *>(&input1);
  uint64_t *i2 = reinterpret_cast<uint64_t *>(&input2);
  uint64_t *o = reinterpret_cast<uint64_t *>(&output);
  if (!div[5])
    div[5] = new Circuit<SIMDCircExec<NetIO>>(
        "/usr/local/include/mpc/circuits/divd.txt", party, simd_circ);
  compute<uint64_t, Circuit<SIMDCircExec<NetIO>>>(i1, i2, o, 1, div[5]);
  return output;
}
bool fcmpEq(double input1, double input2, int op) {
  bool output;
  fcmpEq(&input1, &input2, &output, 1, op);
  return output;
}

} // namespace MPC
