/* nnue.h — network API (format and inference: inference.c). */
#ifndef NNUE_H
#define NNUE_H
#include "ravager.h"

extern bool nnue_loaded, nnue_enabled;
extern const char *nnue_tier;   /* inference tier in use: scalar, avx2, avx512, avx512-vnni */

bool nnue_load_file(const char *path);
bool nnue_load_embedded(void);

/* Search interface: prepare at the root, push before every make_move. */
void nnue_prepare_search(const Board *b);
void nnue_push_move(const Board *b, Move m);
void nnue_push_null(const Board *b);

/* Raw network output (side to move, search scale). */
int32_t nnue_raw_output(const Board *b, bool fresh);
/* Static evaluation for display: material-phase scaled, HCE when disabled. */
int  nnue_evaluate_board(const Board *b);

#endif
