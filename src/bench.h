/* bench.h — perft and the built-in benchmark. */
#ifndef BENCH_H
#define BENCH_H
#include "ravager.h"

uint64_t perft(Board *b, int depth);
void perft_divide(Board *b, int depth);
void run_bench(int depth);

#endif
