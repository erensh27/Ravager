/* Ravager network API. See docs/NNUE.md for the network format. */
#ifndef NNUE_H
#define NNUE_H
#include "ravager.h"

extern bool nnue_loaded, nnue_enabled;
extern const char *nnue_tier;   /* "scalar", "avx2" or "avx512" actually in use */

bool nnue_load_file(const char *path);
bool nnue_load_embedded(void);
void nnue_prepare_search(const Board *b);
void nnue_push_move(const Board *b, Move m);
void nnue_push_null(const Board *b);
int  nnue_eval(const Board *b);
int  nnue_evaluate_board(const Board *b);

/* Verification hooks: logit scaled by 240 and lazy/fresh/scalar cross-checks. */
int32_t  nnue_raw_output(const Board *b, bool fresh);
int32_t  nnue_raw_output_scalar(const Board *b);
bool     nnue_selfcheck(const Board *b);

#endif
