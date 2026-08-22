/* see.c — static exchange evaluation (swap-list algorithm) */

#include "bitboard.h"

static const int see_piece_val[6] = { 100, 325, 350, 500, 975, 10000 };

static Bitboard least_valuable_attacker(const Board *b, Bitboard attackers, int side, int *piece_out) {
    for (int p = PAWN; p <= KING; p++) {
        Bitboard subset = attackers & b->pieces[side][p];
        if (subset) {
            *piece_out = p;
            return subset & (-subset);
        }
    }
    return 0;
}

static int see(Board *b, int to_sq, int target_piece, int from_sq, int atkr_piece) {
    int gain[32];
    int d = 0;

    Bitboard may_xray = b->pieces[WHITE][PAWN]   | b->pieces[BLACK][PAWN]   |
                        b->pieces[WHITE][BISHOP] | b->pieces[BLACK][BISHOP] |
                        b->pieces[WHITE][ROOK]   | b->pieces[BLACK][ROOK]   |
                        b->pieces[WHITE][QUEEN]  | b->pieces[BLACK][QUEEN];

    Bitboard occ = b->occ_all;
    Bitboard attackers = all_attackers_to(b, to_sq, occ);
    Bitboard from_set = (1ULL << from_sq);
    int side = b->side;

    gain[d] = (target_piece != NO_PIECE) ? see_piece_val[target_piece] : 0;

    do {
        d++;
        gain[d] = see_piece_val[atkr_piece] - gain[d-1];

        if ((-gain[d-1] > gain[d])) gain[d] = -gain[d-1]; /* max standing recapture */
        if (gain[d] < 0) break;

        attackers ^= from_set;
        occ       ^= from_set;

        if (from_set & may_xray) {
            attackers |= (rook_attacks(to_sq, occ)   & (b->pieces[WHITE][ROOK]   | b->pieces[BLACK][ROOK]   |
                                                        b->pieces[WHITE][QUEEN]  | b->pieces[BLACK][QUEEN])) & occ;
            attackers |= (bishop_attacks(to_sq, occ) & (b->pieces[WHITE][BISHOP] | b->pieces[BLACK][BISHOP] |
                                                        b->pieces[WHITE][QUEEN]  | b->pieces[BLACK][QUEEN])) & occ;
        }

        side ^= 1;
        from_set = least_valuable_attacker(b, attackers, side, &atkr_piece);
    } while (from_set);

    while (--d)
        gain[d-1] = -((-gain[d-1] > gain[d]) ? -gain[d-1] : gain[d]);

    return gain[0];
}

int see_move(Board *b, Move m) {
    int from = move_from(m), to = move_to(m);
    int target = (b->occ_all & (1ULL << to)) ? (b->piece_on[to] % 6) : NO_PIECE;
    if (move_is_ep(m)) target = PAWN;
    int attacker = b->piece_on[from] % 6;
    return see(b, to, target, from, attacker);
}
