/* tb_syzygy.c — glue between Ravager's Board and the Pyrrhic probe library.
 *
 * Search-time probing uses WDL tables only (cheap, mmap'd, cached by the
 * library). At the root, DTZ is consulted so that wins are actually
 * converted under the 50-move rule. Probes are skipped for positions with
 * castling rights (syzygy tables never contain them) and gated by the UCI
 * SyzygyProbeLimit option.
 */

#include "bitboard.h"
#include "tb/tbprobe.h"

void syzygy_init(const char *path);
bool syzygy_available(void);
int  syzygy_largest(void);

int      syzygy_probe_limit = 6;
bool     syzygy_50move_rule = true;
uint64_t syzygy_hits        = 0;

static bool tb_ok = false;

void syzygy_init(const char *path) {
    tb_ok = path && *path && strcmp(path, "<empty>") != 0;
    if (!tb_ok) { TB_LARGEST = 0; return; }
    tb_init(path);          /* prints its own diagnostics on failure */
    tb_ok = TB_LARGEST > 0;
    if (tb_ok)
        fprintf(stderr, "info string Syzygy ready (%d men), path '%s'\n",
                TB_LARGEST, path);
    else
        fprintf(stderr, "info string no usable Syzygy tables in '%s'\n", path);
}

bool syzygy_available(void) { return tb_ok; }
int  syzygy_largest(void)   { return tb_ok ? TB_LARGEST : 0; }

/* Pack a Ravager board into Pyrrhic's argument list and probe.
 * ep encoding: 0 = none, otherwise square+1 (a1 can never be an ep square). */
static unsigned raw_probe_wdl(const Board *b) {
    unsigned ep = (b->ep_square == NO_SQUARE) ? 0u : (unsigned)b->ep_square + 1u;
    return tb_probe_wdl(
        b->occupancy[WHITE],            b->occupancy[BLACK],
        b->pieces[WHITE][KING]   | b->pieces[BLACK][KING],
        b->pieces[WHITE][QUEEN]  | b->pieces[BLACK][QUEEN],
        b->pieces[WHITE][ROOK]   | b->pieces[BLACK][ROOK],
        b->pieces[WHITE][BISHOP] | b->pieces[BLACK][BISHOP],
        b->pieces[WHITE][KNIGHT] | b->pieces[BLACK][KNIGHT],
        b->pieces[WHITE][PAWN]   | b->pieces[BLACK][PAWN],
        ep, b->side == WHITE);
}

bool syzygy_probe_wdl(const Board *b, int ply, int *score) {
    if (!tb_ok || b->castling_rights != 0) return false;
    int pieces = popcount(b->occ_all);
    if (pieces > TB_LARGEST || pieces > syzygy_probe_limit) return false;

    unsigned r = raw_probe_wdl(b);
    if (r == TB_RESULT_FAILED) return false;
    syzygy_hits++;

    switch ((int)r) {
        case TB_WIN:
            *score = MATE_SCORE - ply - 1; break;
        case TB_LOSS:
            *score = -(MATE_SCORE - ply - 1); break;
        case TB_CURSED_WIN:
        case TB_BLESSED_LOSS:
            *score = syzygy_50move_rule ? 0 : (r == TB_CURSED_WIN ? MATE_SCORE - ply - 1
                                                                  : -(MATE_SCORE - ply - 1));
            break;
        default:
            *score = 0; break;             /* TB_DRAW */
    }
    return true;
}

Move syzygy_probe_root(const Board *b) {
    if (!tb_ok || b->castling_rights != 0) return NO_MOVE;
    int pieces = popcount(b->occ_all);
    if (pieces > TB_LARGEST || pieces > syzygy_probe_limit) return NO_MOVE;

    unsigned ep = (b->ep_square == NO_SQUARE) ? 0u : (unsigned)b->ep_square + 1u;
    unsigned res = tb_probe_root(
        b->occupancy[WHITE],            b->occupancy[BLACK],
        b->pieces[WHITE][KING]   | b->pieces[BLACK][KING],
        b->pieces[WHITE][QUEEN]  | b->pieces[BLACK][QUEEN],
        b->pieces[WHITE][ROOK]   | b->pieces[BLACK][ROOK],
        b->pieces[WHITE][BISHOP] | b->pieces[BLACK][BISHOP],
        b->pieces[WHITE][KNIGHT] | b->pieces[BLACK][KNIGHT],
        b->pieces[WHITE][PAWN]   | b->pieces[BLACK][PAWN],
        (unsigned)b->half_move_clock, ep, b->side == WHITE, NULL);

    if (res == TB_RESULT_FAILED || res == TB_RESULT_CHECKMATE ||
        res == TB_RESULT_STALEMATE)
        return NO_MOVE;

    int from = (int)TB_GET_FROM(res);
    int to   = (int)TB_GET_TO(res);
    int flags = FLAG_NORMAL;
    int promo = 0;

    if (TB_GET_EP(res)) flags = FLAG_EP;
    switch (TB_GET_PROMOTES(res)) {
        case PYRRHIC_FLAG_QPROMO: flags = FLAG_PROMO; promo = PROMO_QUEEN;  break;
        case PYRRHIC_FLAG_RPROMO: flags = FLAG_PROMO; promo = PROMO_ROOK;   break;
        case PYRRHIC_FLAG_BPROMO: flags = FLAG_PROMO; promo = PROMO_BISHOP; break;
        case PYRRHIC_FLAG_NPROMO: flags = FLAG_PROMO; promo = PROMO_KNIGHT; break;
        default: break;
    }

    /* Safety net: the decoded move must exist in our own legal move list. */
    Move m = encode_move(from, to, flags, promo);
    MoveList ml;
    generate_moves((Board*)b, &ml);
    int mover = b->side;
    for (int i = 0; i < ml.count; i++) {
        if (ml.moves[i] != m) continue;
        make_move((Board*)b, m);
        bool legal = !is_in_check((Board*)b, mover);
        unmake_move((Board*)b, m);
        return legal ? m : NO_MOVE;
    }
    return NO_MOVE;
}
