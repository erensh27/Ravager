/* search.h — search driver interface. */
#ifndef SEARCH_H
#define SEARCH_H

#include "ravager.h"

#define TB_WIN_SCORE  30000
#define TB_WIN_BOUND  (TB_WIN_SCORE - 2 * MAX_PLY)
#define EVAL_LIMIT    (TB_WIN_BOUND - 1)

typedef struct {
    int      time[2], inc[2];   /* milliseconds, indexed by colour */
    int      movestogo;
    int      movetime;
    int      depth;
    uint64_t nodes;
    int      mate;              /* moves */
    bool     infinite;
    bool     ponder;
} SearchLimits;

typedef struct {
    Move     best, ponder;
    int      score;             /* side to move at the root */
    int      depth;
    uint64_t nodes;
    int      time_ms;
} SearchResult;

void search_init(void);
void search_set_threads(int n);
int  search_get_threads(void);
void search_clear(void);              /* forget all learned statistics */
/* Runs a complete search; prints info/bestmove lines when search_verbose. */
void search_run(const Board *root, const uint64_t *history, int history_len,
                const SearchLimits *limits, SearchResult *result);
void search_stop(void);
void search_ponderhit(void);

extern int  search_verbose;           /* 0 silences all search output */
extern int  move_overhead_ms;

#endif
