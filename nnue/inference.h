/* Ravager C11 network inference: sparse-threat accumulators and float heads. */
#ifndef RV_INFERENCE_H
#define RV_INFERENCE_H
#include "ravager.h"
#define NET_BYTES 89315360u
bool net_load(const unsigned char *bytes, size_t count);
void net_prepare(const Board *b);
void net_push(const Board *b);
int net_eval(const Board *b, bool fresh);
int net_eval_scalar(const Board *b);
bool net_check(const Board *b);
extern const char *net_tier;
#endif
