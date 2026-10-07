#include "emp-sh2pc/emp-sh2pc.h"
#include "emp-tool/utils/utils.h"
#include "mpc/mpc.h"
#include <fstream>
#include <iostream>

using namespace emp;

namespace MPC
{
  PRG prg;
  int party;
  vector<NetIO *> ios;
  long num_ands = 0;
  float time_in_mpc = 0;
  void *output;
  std::vector<void *> inputStack;

  void setInput(void *p)
  {
    inputStack.push_back(p);
  }

  int getNumGates()
  {
    long x = CircuitExecution::circ_exec->num_and();
    printf("time used in mpc: %f\t", time_in_mpc / 1000000);
    return x - num_ands;
  }
  void setNumGates() { num_ands = CircuitExecution::circ_exec->num_and(); }
  void makeShared(void *arr, int n, int elementSize)
  {
    if (party != 1)
      memset(arr, 0, n * elementSize);
  }

  void send_bool(bool *b, int n)
  {
    ios[0]->send_bool(b, n);
    ios[0]->flush();
  }
  void recv_bool(bool *b, int n) { ios[0]->recv_bool(b, n); }

  void reverse(void *arr, void *arr2, int n, int elementSize)
  {
    for (int i = 0; i < n; ++i)
    {
      if (log2(elementSize) == 0)
      {
        ((int8_t *)arr2)[i] = ((int8_t *)arr)[n - i - 1];
      }
      else if (log2(elementSize) == 1)
      {
        ((int16_t *)arr2)[i] = ((int16_t *)arr)[n - i - 1];
      }
      else if (log2(elementSize) == 2)
      {
        ((int32_t *)arr2)[i] = ((int32_t *)arr)[n - i - 1];
      }
      else if (log2(elementSize) == 3)
      {
        ((int64_t *)arr2)[i] = ((int64_t *)arr)[n - i - 1];
      }
    }
  }

  __attribute__((always_inline)) void store(void *arr, void *element, int n, int elementSize, bool shared,
                                            bool consecutive)
  {
    // auto start = clock_start();
    if (shared)
    {
      if (!consecutive)
      {
        if (elementSize == 1)
          for (int i = 0; i < n; ++i)
            ((Bit *)arr)[i] = ((Bit *)element)[0];
        else
          for (int i = 0; i < n; ++i)
            ((Integer *)arr)[i] = ((Integer *)element)[0];

        // auto time = time_from(start);
        // std::cout << "store shared: " << time / 1000000 <<  "\n";
        return;
      }
    }

    if (elementSize == 1)
    {
      Bit a(((bool *)element)[0], PUBLIC);
      for (int i = 0; i < n; ++i)
        ((Bit *)arr)[i] = a;
    }
    else
    {
      Integer a;
      if (log2(elementSize) == 1)
        a = Integer(elementSize * 8, ((int16_t *)element)[0], PUBLIC);
      else if (log2(elementSize) == 2)
        a = Integer(elementSize * 8, ((int32_t *)element)[0], PUBLIC);
      else if (log2(elementSize) == 3)
        a = Integer(elementSize * 8, ((int64_t *)element)[0], PUBLIC);
      for (int i = 0; i < n; ++i)
      {
        if (consecutive)
          ((Integer *)arr)[i] = Integer(elementSize * 8, ((int64_t *)element)[0] + i, PUBLIC);
        else
          ((Integer *)arr)[i] = a;
      }
    }
    // auto time = time_from(start);
    // std::cout << "store: " << time / 1000000 <<  "\n";
  }

  __attribute__((always_inline)) void updateType(void *a, void *b, int n, int elementSize1, int elementSize2)
  {
    // auto start = clock_start();
    if (log2(elementSize1) == 0)
      {
        for (int i = 0; i < n; ++i)
          if (log2(elementSize2) == 0)
            ((Bit *)a)[i] = ((Bit *)b)[i];
          else
          {
            if (log2(elementSize2) == 1)
              ((Bit *)a)[i] = ((Integer *)b)[i].bits[0];
            else if (log2(elementSize2) == 2)
              ((Bit *)a)[i] = ((Integer *)b)[i].bits[0];
            else if (log2(elementSize2) == 3)
              ((Bit *)a)[i] = ((Integer *)b)[i].bits[0];
          }
      }
      else
      {
        for (int i = 0; i < n; ++i)
        {
          if (log2(elementSize2) == 0)
          {
            // Zero-extend the Bit: bit 0 is b, the rest are public zeros.
            std::vector<Bit> bits(elementSize1 * 8, Bit(false, PUBLIC));
            bits[0] = ((Bit *)b)[i];
            ((Integer *)a)[i] = Integer(bits);
          }
          else
          {
            if (elementSize1 > elementSize2)
            {
              std::vector<Bit> bits(elementSize1 * 8, Bit(false, PUBLIC));
              for (int j = 0; j < elementSize2 * 8; ++j)
                bits[j] = ((Integer *)b)[i].bits[j];
              ((Integer *)a)[i] = Integer(bits);
            }
            else
            {
              std::vector<Bit> bits(elementSize1 * 8);
              for (int j = 0; j < elementSize1 * 8; ++j)
                bits[j] = ((Integer *)b)[i].bits[j];
              ((Integer *)a)[i] = Integer(bits);
            }
          }
        }
      }

    // auto time = time_from(start);
    // std::cout << "update type:  " << time / 1000000 <<  "\n";
  }

  __attribute__((always_inline)) Integer *getInteger(int8_t v)
  {
    Integer *i = new Integer[1];
    i[0] = Integer(8, v, PUBLIC);
    return i;
  }

  __attribute__((always_inline)) Integer *getInteger(int16_t v)
  {
    Integer *i = new Integer[1];
    i[0] = Integer(16, v, PUBLIC);
    return i;
  }
  __attribute__((always_inline)) Integer *getInteger(int32_t v)
  {
    // auto start = clock_start();
    Integer *i = new Integer[1];
    i[0] = Integer(32, v, PUBLIC);
    // auto time = time_from(start);
    // std::cout << "getintptr " << time / 1000000 <<  "\n";
    // time_in_mpc += time;
    return i;
  }
  __attribute__((always_inline)) Integer *getInteger(int64_t v)
  {
    Integer *i = new Integer[1];
    i[0] = Integer(64, v, PUBLIC);
    return i;
  }
  __attribute__((always_inline)) Bit *getBit(bool v)
  {
    // auto start = clock_start();
    Bit *i = new Bit[1];
    i[0] = Bit(v, PUBLIC);
    // auto time = time_from(start);
    // if(time != 0)
    // std::cout << "getbit " << time / 1000000 <<  "\n";
    // time_in_mpc += time;
    return i;
  }

  __attribute__((always_inline)) Integer *createInt(int64_t n)
  {
    // auto start = clock_start();
    Integer *res = new Integer[n];
    // auto time = time_from(start);
    // std::cout << "createint " << time / 1000000 <<  "\n";
    // time_in_mpc += time;
    return res;
  }

  __attribute__((always_inline)) Bit *createBit(int64_t n)
  {
    // auto start = clock_start();
    Bit *res = new Bit[n];
    // auto time = time_from(start);
    // std::cout << "createbit " << time / 1000000 <<  "\n";
    // time_in_mpc += time;
    return res;
  }

  __attribute__((always_inline)) Integer *bitToInt(Bit *b, int size)
  {
    vector<Bit> bits(size, b[0]);
    Integer *i = new Integer[1];
    i[0] = Integer(bits);
    return i;
  }

  // Zero-extends a Bit: bit 0 is b, the remaining bits are public zeros.
  __attribute__((always_inline)) Integer *bitToIntZext(Bit *b, int size)
  {
    vector<Bit> bits(size, Bit(false, PUBLIC));
    bits[0] = b[0];
    Integer *i = new Integer[1];
    i[0] = Integer(bits);
    return i;
  }

  // Widens an Integer to size bits, with sign or zero extension.
  __attribute__((always_inline)) Integer *extInt(Integer *x, int size, bool sign)
  {
    Integer *i = new Integer[1];
    i[0] = x[0];
    i[0].resize(size, sign);
    return i;
  }

  __attribute__((always_inline)) Integer *gep(Integer *ptr, int64_t idx)
  {
    if (ptr == nullptr)
    {
      printf("gep ptr is nullptr\n");
      exit(1);
    }
    return ptr + idx;
  }

  __attribute__((always_inline)) Bit *gep(Bit *ptr, int64_t idx)
  {
    if (ptr == nullptr)
    {
      printf("gep ptr is nullptr\n");
      exit(1);
    }
    return ptr + idx;
  }

  __attribute__((always_inline)) void store(Integer *ints, Integer &i)
  {
    // auto start = clock_start();
    if (ints == nullptr)
    {
      printf("store ptr is nullptr\n");
      exit(1);
    }
    ints[0] = i;
    // auto time = time_from(start);
    // if(time != 0)
    // std::cout << "store int " << time / 1000000 <<  "\n";
    // time_in_mpc += time;
    // printf("store %d\n", ints[0].size());
  }

  __attribute__((always_inline)) void store(Bit *bits, Bit &b)
  {
    // auto start = clock_start();
    bits[0] = b;
    // auto time = time_from(start);
    // if(time != 0)
    // std::cout << "store bit " << time / 1000000 <<  "\n";
    // time_in_mpc += time;
  }

  void *getInput(int op)
  {
    if (op < inputStack.size())
    {
      void *ret = inputStack[op];
      if (op == inputStack.size() - 1)
        inputStack.clear();
      return ret;
    }
    return nullptr;
  }

  __attribute__((always_inline)) Bit *getBitPtr(bool *b, int n)
  {
    Bit *bits[2];
    bits[0] = new Bit[n];
    bits[1] = new Bit[n];
    int otherParty = party == 1 ? 2 : 1;
    for (int i = 0; i < n; ++i)
    {
      bits[party - 1][i] = Bit(b[i], party);
      bits[otherParty - 1][i] = Bit(0, otherParty);
      bits[0][i] ^= bits[1][i];
    }
    delete[] bits[1];
    return bits[0];
  }

  template <typename T>
  __attribute__((always_inline)) Integer *getIntPtr(T *values, int n)
  {
    // auto start = clock_start();
    Integer *ints[2];
    ints[0] = new Integer[n];
    ints[1] = new Integer[n];
    int otherParty = party == 1 ? 2 : 1;
    for (int i = 0; i < n; ++i)
    {
      ints[party - 1][i] = Integer(sizeof(T) * 8, values[i], party);
      ints[otherParty - 1][i] = Integer(sizeof(T) * 8, 0, otherParty);
      ints[0][i] ^= ints[1][i];
    }
    delete[] ints[1];
    // auto time = time_from(start);
    // std::cout << "getintptr " << time / 1000000 <<  "\n";
    // time_in_mpc += time;
    return ints[0];
  }

  // Writes XOR shares of vals back to the caller's array. elementBits is the
  // element width in bits, as VectorMPCLink passes it.
  __attribute__((always_inline)) void writeArg(Integer *vals, void *arg, int n, int elementBits)
  {
    for (int i = 0; i < n; ++i)
    {
      if (elementBits == 8)
        ((int8_t *)arg)[i] = vals[i].reveal<int32_t>(XOR);
      else if (elementBits == 16)
        ((int16_t *)arg)[i] = vals[i].reveal<int32_t>(XOR);
      else if (elementBits == 32)
        ((int32_t *)arg)[i] = vals[i].reveal<int32_t>(XOR);
      else if (elementBits == 64)
        ((int64_t *)arg)[i] = vals[i].reveal<int64_t>(XOR);
    }
  }

  template Integer *getIntPtr<int8_t>(int8_t *values, int n);
  template Integer *getIntPtr<int16_t>(int16_t *values, int n);
  template Integer *getIntPtr<int32_t>(int32_t *values, int n);
  template Integer *getIntPtr<int64_t>(int64_t *values, int n);

  __attribute__((always_inline)) void reduction(void *arr, int n, int elementSize, void *res, int op,
                                                bool shared)
  {
    if (!shared)
    {
      // if (elementSize == 0)
      //   redHelper<bool>((bool *)arr, (bool *)res, n, op);
      // else if (log2(elementSize) == 0)
      //   redHelper<int8_t>((int8_t *)arr, (int8_t *)res, n, op);
      // else if (log2(elementSize) == 1)
      //   redHelper<int16_t>((int16_t *)arr, (int16_t *)res, n, op);
      // else if (log2(elementSize) == 2)
      //   redHelper<int32_t>((int32_t *)arr, (int32_t *)res, n, op);
      // else if (log2(elementSize) == 3)
      //   redHelper<int64_t>((int64_t *)arr, (int64_t *)res, n, op);
      return;
    }
    // auto start = clock_start();
    if (elementSize == 0)
    {
      Bit result = ((Bit *)arr)[0];
      for (int i = 1; i < n; ++i)
      {
        if (op == reductionOp::OR)
          result = result | (((Bit *)arr)[i]);
        else
        {
          std::cout << "reduction for elementsize " << 0 << " op " << op
                    << " not implemented\n";
          exit(-1);
        }
      }
      ((Bit *)res)[0] = result;

      // auto time = time_from(start);
      // std::cout << "reduction " << time / 1000000 <<  "\n";
      // time_in_mpc += time;
      return;
    }
    Integer result = ((Integer *)arr)[0];
    for (int i = 1; i < n; ++i)
    {
      if (op == reductionOp::ADD)
        result = result + (((Integer *)arr)[i]);
      else if (op == reductionOp::MUL)
        result = result * (((Integer *)arr)[i]);
      else if (op == reductionOp::MAX)
        result = result.select(result < (((Integer *)arr)[i]), ((Integer *)arr)[i]);
      else if (op == reductionOp::MIN)
        result = result.select(result > (((Integer *)arr)[i]), ((Integer *)arr)[i]);
      else if (op == reductionOp::OR)
        result = result | (((Integer *)arr)[i]);
    }
    ((Integer *)res)[0] = result;
    // auto time = time_from(start);
    // std::cout << "reduction 403: " << time / 1000000 <<  "\n";
    // time_in_mpc += time;
  }

  __attribute__((always_inline)) void divide(Integer *a, Integer *b, Integer *res, int n)
  {
    for (int i = 0; i < n; ++i)
      res[i] = a[i] / b[i];
  }

  __attribute__((always_inline)) void max(Integer *a, Integer *b, Integer *res, int n)
  {
    for (int i = 0; i < n; ++i)
    {
      Bit cmp = a[i] > b[i];
      res[i] = b[i].select(cmp, a[i]);
    }
  }

  __attribute__((always_inline)) void min(Integer *a, Integer *b, Integer *res, int n)
  {
    for (int i = 0; i < n; ++i)
    {
      Bit cmp = a[i] > b[i];
      res[i] = a[i].select(cmp, b[i]);
    }
  }

  __attribute__((always_inline)) void select(Bit *trueVal, Bit *falseVal, Bit *condition, Bit *res, int n)
  {
    for (int i = 0; i < n; ++i)
      res[i] =
          (falseVal[i]).select(condition[i], trueVal[i]);
  }

  __attribute__((always_inline)) void select(Integer *trueVal, Integer *falseVal, Bit *condition, Integer *res, int n)
  {
    for (int i = 0; i < n; ++i) {
      if (falseVal[i].size() != trueVal[i].size()) {
        fprintf(stderr, "select size mismatch i=%d n=%d falseVal.size=%zu trueVal.size=%zu\n",
                i, n, falseVal[i].size(), trueVal[i].size());
        fflush(stderr);
      }
      res[i] =
          (falseVal[i]).select(condition[i], trueVal[i]);
    }
  }

  int rand_int32()
  {
    bool *b = new bool[32];
    prg.random_bool(b, 32);
    int x = bool_to_int<int32_t>(b);
    return x;
  }
  int random(int bitlength)
  {
    bool *b = new bool[32];
    memset(b, 0, 32);
    prg.random_bool(b, bitlength);
    return bool_to_int<int32_t>(b);
  }

  template <bool>
  __attribute__((always_inline)) bool *reveal(bool *data, int num, int p)
  {
    if (party == p)
    {
      bool *dataRecv = (bool *)malloc(num * sizeof(bool));
      ios[0]->recv_bool(dataRecv, num);
      for (int i = 0; i < num; ++i)
      {
        data[i] ^= dataRecv[i];
      }
      free(dataRecv);
    }
    else
    {
      ios[0]->send_bool(data, num);
      ios[0]->flush();
    }

    return data;
  }
  template <typename T>
  __attribute__((always_inline)) T *reveal(T *data, int num, int p)
  {
    if (party == p)
    {
      T *dataRecv = (T *)malloc(num * sizeof(T));
      ios[0]->recv_data(dataRecv, num * sizeof(T));
      for (int i = 0; i < num; ++i)
      {
        data[i] ^= dataRecv[i];
      }
      free(dataRecv);
    }
    else
    {
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

  __attribute__((always_inline)) bool reveal(Bit *b)
  {
    bool r = b->reveal(XOR);
    return r;
  }
  __attribute__((always_inline)) int64_t reveal(Integer *b)
  {
    int64_t r = b->reveal<int64_t>(XOR);
    return r;
  }

  template <typename T>
  __attribute__((always_inline))
  T
  reveal(T data, int p)
  {
    T rev;
    if (party == ALICE)
    {
      ios[0]->send_data(&data, sizeof(T));
      ios[0]->flush();
      ios[0]->recv_data(&rev, sizeof(T));
    }
    else
    {
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

  __attribute__((always_inline)) bool isAlice() { return party == 1; }

  void setup()
  {
    int port;
    std::cout << "party \n";
    std::cin >> party;
    std::cout << "port \n";
    std::cin >> port;
    setup(party, port);
  }

  void setup(int p, int port)
  {
    MPC::party = p;
    int threads = 1;
    for (int i = 0; i < threads; ++i)
      ios.push_back(
          new NetIO(party == ALICE ? nullptr : "127.0.0.1", port, true));
    setup_semi_honest(ios[0], party);
  }

  void storeConst(void *a, int8_t *b, int32_t n, int m)
  {
    int8_t *tmp = (int8_t *)a;
    for (int i = 0; i < n / m; ++i)
    {
      for (int j = 0; j < m; ++j)
      {
        tmp[i * m + j] = b[j];
      }
    }
  }

  void finish() { finalize_semi_honest(); }

  __attribute__((always_inline)) Bit *icmp(Integer &input1, Integer &input2, int op)
  {
    Bit *output = new Bit[1];
    switch (op)
    {
    case cmp::EQ:
      output[0] = (input1 == input2);
      break;
    case cmp::GT:
      output[0] = (input1 > input2);
      break;
    case cmp::GE:
      output[0] = (input1 >= input2);
      break;
    case cmp::NE:
      output[0] = (input1 != input2);
      break;
    default:
      break;
    }
    return output;
  }

  __attribute__((always_inline)) void icmp(Integer *input1, Integer *input2, Bit *output, int n, int op)
  {
    // auto start = clock_start();
    for (int i = 0; i < n; ++i)
    {
      switch (op)
      {
      case cmp::EQ:
        output[i] = (input1[i] == input2[i]);
        break;
      case cmp::GT:
        output[i] = (input1[i] > input2[i]);
        break;
      case cmp::GE:
        output[i] = (input1[i] >= input2[i]);
        break;
      case cmp::NE:
        output[i] = (input1[i] != input2[i]);
        break;
      default:
        break;
      }
    }
    // auto time = time_from(start);
    // std::cout << "icmp arr " << time / 1000000 <<  "\n";
    // time_in_mpc += time;
  }

  __attribute__((always_inline)) Integer *andInt(Integer &input1, Integer &input2)
  {
    Integer *r = new Integer[1];
    if (input1.size() == 0)
      r[0] = input2;
    else if (input2.size() == 0)
      r[0] = input1;
    else
      r[0] = (input1 & input2);
    return r;
  }

  __attribute__((always_inline)) void andInt(Integer *input1, Integer *input2, Integer *output, int n)
  {
    for (int i = 0; i < n; ++i)
      output[i] = input1[i] & input2[i];
  }
  __attribute__((always_inline)) Integer *xorInt(Integer &input1, Integer &input2)
  {
    Integer *r = new Integer[1];
    if (input1.size() == 0)
      r[0] = input2;
    else if (input2.size() == 0)
      r[0] = input1;
    else
      r[0] = (input1 ^ input2);
    return r;
  }
  __attribute__((always_inline)) void xorInt(Integer *input1, Integer *input2, Integer *output, int n)
  {
    for (int i = 0; i < n; ++i)
      output[i] = input1[i] ^ input2[i];
  }
  __attribute__((always_inline)) Bit *andBit(Bit &input1, Bit &input2)
  {
    // auto start = clock_start();
    Bit *r = new Bit[1];
    r[0] = (input1 & input2);
    // auto time = time_from(start);
    // if(time != 0)
    // std::cout << "and bit " << time / 1000000 <<  "\n";
    // time_in_mpc += time;
    return r;
  }
  __attribute__((always_inline)) void andBit(Bit *input1, Bit *input2, Bit *output, int n)
  {
    for (int i = 0; i < n; ++i)
      output[i] = input1[i] & input2[i];
  }
  __attribute__((always_inline)) Bit *xorBit(Bit &input1, Bit &input2)
  {
    // auto start = clock_start();
    Bit *r = new Bit[1];
    r[0] = (input1 ^ input2);
    // auto time = time_from(start);
    // if(time != 0)
    // std::cout << "xor bit " << time / 1000000 <<  "\n";
    // time_in_mpc += time;
    return r;
  }

  __attribute__((always_inline)) void notBit(Bit *input, Bit *output, int N)
  {
    for (int i = 0; i < N; ++i)
      output[i] = !input[i];
  }

  __attribute__((always_inline)) void notInt(Integer *input, Integer *output, int N)
  {
    for (int i = 0; i < N; ++i)
      output[i] = input[i] ^ Integer((1 << (input[i].size() - 1)) - 1, PUBLIC);
  }

  __attribute__((always_inline)) void xorBit(Bit *input1, Bit *input2, Bit *output, int n)
  {
    for (int i = 0; i < n; ++i)
      output[i] = input1[i] ^ input2[i];
  }
  __attribute__((always_inline)) Integer *add(Integer &input1, Integer &input2)
  {
    Integer *r = new Integer[1];
    r[0] = (input1 + input2);
    return r;
  }
  __attribute__((always_inline)) void add(Integer *input1, Integer *input2, Integer *output, int n)
  {
    for (int i = 0; i < n; ++i)
      output[i] = input1[i] + input2[i];
  }
  __attribute__((always_inline)) Integer *sub(Integer &input1, Integer &input2)
  {
    Integer *r = new Integer[1];
    r[0] = (input1 - input2);
    return r;
  }
  __attribute__((always_inline)) void sub(Integer *input1, Integer *input2, Integer *output, int n)
  {
    for (int i = 0; i < n; ++i)
      output[i] = input1[i] - input2[i];
  }
  __attribute__((always_inline)) Integer *mult(Integer &input1, Integer &input2)
  {
    Integer *r = new Integer[1];
    r[0] = (input1 * input2);
    return r;
  }
  __attribute__((always_inline)) void mult(Integer *input1, Integer *input2, Integer *output, int n)
  {
    for (int i = 0; i < n; ++i)
      output[i] = input1[i] * input2[i];
  }
  __attribute__((always_inline)) Integer *div(Integer &input1, Integer &input2)
  {
    Integer *r = new Integer[1];
    r[0] = (input1 / input2);
    return r;
  }
  __attribute__((always_inline)) void div(Integer *input1, Integer *input2, Integer *output, int n)
  {
    for (int i = 0; i < n; ++i)
      output[i] = input1[i] / input2[i];
  }

  __attribute__((always_inline)) void destroy(Integer *ptr)
  {
    delete[] ptr;
  }

  __attribute__((always_inline)) void destroy(Bit *ptr)
  {
    delete[] ptr;
  }

} // namespace MPC
