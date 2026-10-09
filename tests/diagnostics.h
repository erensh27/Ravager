#ifndef RAVAGER_DIAGNOSTICS_H
#define RAVAGER_DIAGNOSTICS_H
#include "ravager.h"
uint64_t perft(Board *b, int depth);
void perft_divide(Board *b, int depth);
void run_bench(Board *board);
#endif
