/* tb_syzygy.h — Ravager-side adapter for Pyrrhic (Syzygy WDL/DTZ probing). */
#ifndef TB_SYZYGY_H
#define TB_SYZYGY_H

#include "ravager.h"

void syzygy_init(const char *path);      /* (re)load tables from path(s)  */
bool syzygy_available(void);             /* any table loaded?             */

extern int  syzygy_probe_limit;          /* UCI: only probe at <= N men   */
extern bool syzygy_50move_rule;          /* UCI: respect blessed/cursed   */
/* Search-time WDL probe. Returns true and sets *wdl to 1 (win), 0 (draw)
 * or -1 (loss) for the side to move, honouring Syzygy50MoveRule. */
bool syzygy_probe_wdl(const Board *b, int *wdl);

/* Root DTZ probe. Returns the tablebase best move in Ravager encoding, or
 * NO_MOVE when unavailable/out of range. Respects the 50-move rule. */
Move syzygy_probe_root(const Board *b);

#endif
