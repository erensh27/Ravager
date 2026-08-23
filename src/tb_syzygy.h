/* tb_syzygy.h — Ravager-side adapter for Pyrrhic (Syzygy WDL/DTZ probing). */
#ifndef TB_SYZYGY_H
#define TB_SYZYGY_H

#include "ravager.h"

void syzygy_init(const char *path);      /* (re)load tables from path(s)  */
bool syzygy_available(void);             /* any table loaded?             */
int  syzygy_largest(void);               /* TB_LARGEST (max pieces known) */

extern int  syzygy_probe_limit;          /* UCI: only probe at <= N men   */
extern bool syzygy_50move_rule;          /* UCI: respect blessed/cursed   */
extern uint64_t syzygy_hits;             /* successful search probes      */

/* Search-time WDL probe. Returns true and fills *score (side-to-move POV,
 * mate scale) when the position is decided by the tablebases. */
bool syzygy_probe_wdl(const Board *b, int ply, int *score);

/* Root DTZ probe. Returns the tablebase best move in Ravager encoding, or
 * NO_MOVE when unavailable/out of range. Respects the 50-move rule. */
Move syzygy_probe_root(const Board *b);

#endif
