/* see.c — static exchange evaluation.
 *
 * see_ge() answers "does this move win at least `threshold`?" with the
 * threshold-based swap algorithm, following the complete recapture sequence
 * including x-ray attackers revealed behind each capturer. */

#include "bitboard.h"

/* Search-scale piece values (the network's units). */
const int see_values[7] = { 233, 446, 446, 716, 1253, 0, 0 };

static int captured_value(const Board *b, Move m) {
    if (move_is_castle(m)) return 0;
    if (move_is_ep(m)) return see_values[PAWN];
    int to = move_to(m);
    int v = (b->occ_all & (1ULL << to)) ? see_values[b->piece_on[to] % 6] : 0;
    if (move_is_promo(m)) {
        static const int promo_piece[] = {KNIGHT, BISHOP, ROOK, QUEEN};
        v += see_values[promo_piece[move_promo(m)]] - see_values[PAWN];
    }
    return v;
}

bool see_ge(const Board *b, Move m, int threshold) {
    if (move_is_castle(m)) return 0 >= threshold;
    int from = move_from(m), to = move_to(m);

    int next = b->piece_on[from] % 6;
    if (move_is_promo(m)) {
        static const int promo_piece[] = {KNIGHT, BISHOP, ROOK, QUEEN};
        next = promo_piece[move_promo(m)];
    }
    int balance = captured_value(b, m) - threshold;
    if (balance < 0) return false;          /* even a free capture falls short */
    balance -= see_values[next];
    if (balance >= 0) return true;          /* losing the mover still passes */

    Bitboard bishops = b->pieces[WHITE][BISHOP] | b->pieces[BLACK][BISHOP] |
                       b->pieces[WHITE][QUEEN]  | b->pieces[BLACK][QUEEN];
    Bitboard rooks   = b->pieces[WHITE][ROOK]   | b->pieces[BLACK][ROOK]   |
                       b->pieces[WHITE][QUEEN]  | b->pieces[BLACK][QUEEN];
    Bitboard occ = (b->occ_all ^ (1ULL << from)) | (1ULL << to);
    if (move_is_ep(m)) occ ^= 1ULL << ((b->side == WHITE) ? to - 8 : to + 8);

    Bitboard attackers = all_attackers_to(b, to, occ) & occ;
    int kings[2] = {lsb(b->pieces[WHITE][KING]), lsb(b->pieces[BLACK][KING])};
    Bitboard pinners[2];
    for (int c = 0; c < 2; c++)
        pinners[c] = ((rook_attacks(kings[c], 0) & rooks) |
                      (bishop_attacks(kings[c], 0) & bishops)) & b->occupancy[c ^ 1] & ~(1ULL << to);
    int side = b->side ^ 1;

    for (;;) {
        Bitboard mine = attackers & b->occupancy[side];
        /* Recompute blockers as exchanges uncover pins or remove pinners.
         * A pinned recapture is legal only along its king's pin ray. */
        for (Bitboard pins = pinners[side] & occ; pins; pins &= pins - 1) {
            int sniper = lsb(pins);
            Bitboard blockers = between_table[kings[side]][sniper] & occ;
            if (blockers && !(blockers & (blockers - 1)) &&
                !(ray_table[kings[side]][sniper] & (1ULL << to)))
                mine &= ~blockers;
        }
        if (!mine) break;
        for (next = PAWN; next <= KING; next++)
            if (mine & b->pieces[side][next]) break;
        Bitboard pick = mine & b->pieces[side][next];
        occ ^= pick & -pick;
        if (next == PAWN || next == BISHOP || next == QUEEN)
            attackers |= bishop_attacks(to, occ) & bishops;
        if (next == ROOK || next == QUEEN)
            attackers |= rook_attacks(to, occ) & rooks;
        attackers &= occ;
        side ^= 1;
        balance = -balance - 1 - see_values[next];
        if (balance >= 0) {
            /* A king may only recapture when nothing defends the square. */
            if (next == KING && (attackers & b->occupancy[side])) side ^= 1;
            break;
        }
    }
    return side != b->side;
}
