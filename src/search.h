/* search.h */
#ifndef SEARCH_H
#define SEARCH_H

#include "ravager.h"

void search_iterative_deepening(Board *b);
void search_stop(void);
void search_reset_tables(void);
void init_lmr_table(void);
void search_set_game_history(const uint64_t *h, int len);
void eval_clear_pawn_hash(void);

/* set by the UCI go parser */
extern volatile int search_soft_ms;
extern volatile int search_hard_ms;
extern int search_max_depth;
extern int search_verbose;   /* 0 suppresses info lines (datagen) */
extern uint64_t search_nodes;
extern Move search_root_best;

#endif
