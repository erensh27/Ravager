/* inference.h — network inference internals (see inference.c). */
#ifndef INFERENCE_H
#define INFERENCE_H
#include "ravager.h"

#define NET_BYTES 89315360u

extern const char *net_tier;
bool net_load(const unsigned char *bytes, size_t count);
void net_prepare(const Board *b);
void net_push(const Board *b);
int  net_eval(const Board *b, bool fresh);

#endif
