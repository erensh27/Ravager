/* tt.h — shared transposition table. */
#ifndef TT_H
#define TT_H

#include "ravager.h"

#define SCORE_NONE 32001   /* "no value" marker for scores and static evals */

typedef struct {
    int  score;      /* search score, already adjusted for ply; SCORE_NONE if absent */
    int  eval;       /* raw static evaluation; SCORE_NONE if absent */
    Move move;
    int  depth;
    int  bound;      /* BOUND_NONE, BOUND_EXACT, BOUND_LOWER or BOUND_UPPER */
    bool pv;         /* the node was ever searched as part of a PV */
} TTHit;

#define BOUND_NONE 3

bool tt_alloc(int mb);
void tt_clear(void);
void tt_new_search(void);
bool tt_probe(uint64_t hash, int ply, TTHit *hit);
void tt_store(uint64_t hash, int ply, Move move, int score, int eval,
              int bound, int depth, bool pv);
int  tt_hashfull(void);
Move tt_probe_move(uint64_t hash);
void tt_prefetch(uint64_t hash);

#endif
