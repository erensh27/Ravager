/* tt.h — transposition table */
#ifndef TT_H
#define TT_H

#include "ravager.h"

void tt_alloc(int mb);
void tt_clear(void);
void tt_store(uint64_t hash, int score, Move move, int depth, int bound, int ply);
bool tt_probe(uint64_t hash, int *score, Move *move, int *depth, int *bound, int ply);
int  tt_hashfull(void);
void tt_age_step(void);
Move tt_probe_move(uint64_t hash);

#endif
