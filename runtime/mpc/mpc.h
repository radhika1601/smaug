#ifndef MATH_OPS_H
#define MATH_OPS_H
#include <stdint.h>

namespace MPC {
extern int party;
enum cmp { EQ = 1, GT = 2, GE = 3, NE = 4 };

enum reductionOp { ADD = 1, MUL = 2, MAX = 3, MIN = 4, OR = 5 };
int getNumGates();
void setNumGates();
bool isAlice();
void reverse(void *arr, void *arr2, int n, int elementSize);
void send_bool(bool *b, int n);
void recv_bool(bool *b, int n);
void makeShared(void *arr, int n, int elementSize);
void store(void *arr, void *element, int n, int elementSize, bool shared = true,
           bool consecutive = false);
void updateType(void *a, void *b, int n, int elementSize1, int elementSize2);
void divide(void *a, void *b, int n, int elementSize, void *res,
            bool shared = true);
void min(void *a, void *b, int n, int elementSize, void *res, bool shared);
void max(void *a, void *b, int n, int elementSize, void *res, bool shared);

void select(void *trueVal, void *falseVal, bool *condition, int n,
            int elementSize, void *res, bool shared = true);
void reduction(void *arr, int n, int elementSize, void *res, int op,
               bool shared = true);

int rand_int32();
int random(int bitlength);
template <typename T> T *reveal(T *data, int num, int p);
template <typename T> T reveal(T data, int p);
void setup(int party, int port);
void setup();
void loadStoreHelper(bool *vals, int64_t idx, int32_t n, bool *andOutput);
void load(void *arr, int32_t idx, void *res, int n, int elementSize,
          bool isPublic);
void store(void *arr, void *val, int32_t idx, int32_t n, int32_t elementSize,
           bool isPublic = false);
void storeConst(void *a, int8_t *b, int32_t n, int m);

void loadStoreHelper(bool *vals, int64_t idx, int32_t n, bool *andOutput);

bool andBool(bool input1, bool input2);
int8_t andI8(int8_t input1, int8_t input2);
int32_t andI32(int32_t input1, int32_t input2);
int64_t andI64(int64_t input1, int64_t input2);
int16_t andI16(int16_t input1, int16_t input2);
int8_t andI8(int8_t input1, int8_t input2);
int8_t addI8(int8_t input1, int8_t input2);
int32_t addI32(int32_t input1, int32_t input2);
int64_t addI64(int64_t input1, int64_t input2);
int16_t addI16(int16_t input1, int16_t input2);
int8_t subI8(int8_t input1, int8_t input2);
int32_t subI32(int32_t input1, int32_t input2);
int64_t subI64(int64_t input1, int64_t input2);
int16_t subI16(int16_t input1, int16_t input2);
int8_t multI8(int8_t input1, int8_t input2);
int32_t multI32(int32_t input1, int32_t input2);
int64_t multI64(int64_t input1, int64_t input2);
int16_t multI16(int16_t input1, int16_t input2);
int8_t divI8(int8_t input1, int8_t input2);
int32_t divI32(int32_t input1, int32_t input2);
int64_t divI64(int64_t input1, int64_t input2);
int16_t divI16(int16_t input1, int16_t input2);
bool icmpEqI8(int8_t input1, int8_t input2, int op);
bool icmpEqI32(int32_t input1, int32_t input2, int op);
bool icmpEqI64(int64_t input1, int64_t input2, int op);
bool icmpEqI16(int16_t input1, int16_t input2, int op);
void divF(float *input1, float *input2, float *output, int n);
void addF(float *input1, float *input2, float *output, int n);
void subF(float *input1, float *input2, float *output, int n);
void multF(float *input1, float *input2, float *output, int n);
void fcmpEq(float *input1, float *input2, bool *output, int n, int op);
float addF(float input1, float input2);
float subF(float input1, float input2);
float multF(float input1, float input2);
float divF(float input1, float input2);
bool fcmpEq(float input1, float input2, int op);

void divF(double *input1, double *input2, double *output, int n);
void addF(double *input1, double *input2, double *output, int n);
void subF(double *input1, double *input2, double *output, int n);
void multF(double *input1, double *input2, double *output, int n);
void fcmpEq(double *input1, double *input2, bool *output, int n, int op);
double addF(double input1, double input2);
double subF(double input1, double input2);
double multF(double input1, double input2);
double divF(double input1, double input2);
bool fcmpEq(double input1, double input2, int op);

void andBool(bool *input1, bool *input2, bool *output, int n,
             bool bothPrivate = true);
void andI8(int8_t *input1, int8_t *input2, int8_t *output, int n,
           bool bothPrivate = true);
void andI16(int16_t *input1, int16_t *input2, int16_t *output, int n,
            bool bothPrivate = true);
void andI32(int32_t *input1, int32_t *input2, int32_t *output, int n,
            bool bothPrivate = true);
void andI64(int64_t *input1, int64_t *input2, int64_t *output, int n,
            bool bothPrivate = true);
void xorBool(bool *input1, bool *input2, bool *output, int n,
             bool bothSame = true);
void xorI8(int8_t *input1, int8_t *input2, int8_t *output, int n,
           bool bothSame = true);
void xorI16(int16_t *input1, int16_t *input2, int16_t *output, int n,
            bool bothSame = true);
void xorI32(int32_t *input1, int32_t *input2, int32_t *output, int n,
            bool bothSame = true);
void xorI64(int64_t *input1, int64_t *input2, int64_t *output, int n,
            bool bothSame = true);
void addI8(int8_t *input1, int8_t *input2, int8_t *output, int n);
void addI16(int16_t *input1, int16_t *input2, int16_t *output, int n);
void addI32(int32_t *input1, int32_t *input2, int32_t *output, int n);
void addI64(int64_t *input1, int64_t *input2, int64_t *output, int n);
void subI8(int8_t *input1, int8_t *input2, int8_t *output, int n);
void subI16(int16_t *input1, int16_t *input2, int16_t *output, int n);
void subI32(int32_t *input1, int32_t *input2, int32_t *output, int n);
void subI64(int64_t *input1, int64_t *input2, int64_t *output, int n);
void multI8(int8_t *input1, int8_t *input2, int8_t *output, int n);
void multI16(int16_t *input1, int16_t *input2, int16_t *output, int n);
void multI32(int32_t *input1, int32_t *input2, int32_t *output, int n);
void multI64(int64_t *input1, int64_t *input2, int64_t *output, int n);

void icmpEqI8(int8_t *input1, int8_t *input2, bool *output, int n, int op);
void icmpEqI16(int16_t *input1, int16_t *input2, bool *output, int n, int op);
void icmpEqI32(int32_t *input1, int32_t *input2, bool *output, int n, int op);
void icmpEqI64(int64_t *input1, int64_t *input2, bool *output, int n, int op);

void finish();
} // namespace MPC

#endif // MATH_OPS_H
