/* movegen.c — pseudo-legal move generation (Andscacs/Ethereal tradition).
 *
 * Moves are validated by the caller: after make_move(), test
 * is_in_check(board, mover). This drops the per-node pin and check-mask
 * work that a fully-legal generator pays for on every node; only the moves
 * that survive move ordering pay for their legality test. Castling is still
 * generated fully legal (its attack tests are part of the rules). */

#include "bitboard.h"

static void add_move(MoveList *ml, Move m) { ml->moves[ml->count++] = m; }

static void add_pawn_promotions(MoveList *ml, int from, int to, int flags) {
    add_move(ml, encode_move(from, to, flags, PROMO_QUEEN));
    add_move(ml, encode_move(from, to, flags, PROMO_KNIGHT));
    add_move(ml, encode_move(from, to, flags, PROMO_ROOK));
    add_move(ml, encode_move(from, to, flags, PROMO_BISHOP));
}

static void gen_castling(const Board *b, MoveList *ml) {
    int side = b->side, opp = side ^ 1;
    if (side == WHITE) {
        if ((b->castling_rights & CASTLE_WK) &&
            !(b->occ_all & 0x60ULL) &&
            (b->pieces[WHITE][ROOK] & (1ULL << H1)) &&
            !is_square_attacked(b, E1, opp) &&
            !is_square_attacked(b, F1, opp) &&
            !is_square_attacked(b, G1, opp))
            add_move(ml, encode_move(E1, G1, FLAG_CASTLE, 0));
        if ((b->castling_rights & CASTLE_WQ) &&
            !(b->occ_all & 0xEULL) &&
            (b->pieces[WHITE][ROOK] & (1ULL << A1)) &&
            !is_square_attacked(b, E1, opp) &&
            !is_square_attacked(b, D1, opp) &&
            !is_square_attacked(b, C1, opp))
            add_move(ml, encode_move(E1, C1, FLAG_CASTLE, 0));
    } else {
        if ((b->castling_rights & CASTLE_BK) &&
            !(b->occ_all & 0x6000000000000000ULL) &&
            (b->pieces[BLACK][ROOK] & (1ULL << H8)) &&
            !is_square_attacked(b, E8, opp) &&
            !is_square_attacked(b, F8, opp) &&
            !is_square_attacked(b, G8, opp))
            add_move(ml, encode_move(E8, G8, FLAG_CASTLE, 0));
        if ((b->castling_rights & CASTLE_BQ) &&
            !(b->occ_all & 0x0E00000000000000ULL) &&
            (b->pieces[BLACK][ROOK] & (1ULL << A8)) &&
            !is_square_attacked(b, E8, opp) &&
            !is_square_attacked(b, D8, opp) &&
            !is_square_attacked(b, C8, opp))
            add_move(ml, encode_move(E8, C8, FLAG_CASTLE, 0));
    }
}

void generate_moves(Board *b, MoveList *ml) {
    ml->count = 0;
    int side = b->side, opp = side ^ 1;
    Bitboard own   = b->occupancy[side];
    Bitboard enemy = b->occupancy[opp];
    Bitboard occ   = b->occ_all;
    (void)enemy;
    Bitboard empty = ~occ;
    int up = (side == WHITE) ? 8 : -8;

    /* King */
    int king_sq = lsb(b->pieces[side][KING]);
    Bitboard km = king_attack_table[king_sq] & ~own;
    while (km) add_move(ml, encode_move(king_sq, lsb_pop(&km), FLAG_NORMAL, 0));
    gen_castling(b, ml);

    /* Pawns */
    Bitboard pawns = b->pieces[side][PAWN];
    Bitboard promo_rank = (side == WHITE) ? RANK_7 : RANK_2;
    Bitboard pawns_promo  = pawns & promo_rank;
    Bitboard pawns_normal = pawns & ~promo_rank;

    Bitboard one = (side == WHITE) ? (pawns_normal << 8) : (pawns_normal >> 8);
    one &= empty;
    Bitboard two = (side == WHITE) ? ((one & RANK_3) << 8) : ((one & RANK_6) >> 8);
    two &= empty;
    while (one) { int to = lsb_pop(&one); add_move(ml, encode_move(to - up, to, FLAG_NORMAL, 0)); }
    while (two) { int to = lsb_pop(&two); add_move(ml, encode_move(to - 2 * up, to, FLAG_NORMAL, 0)); }

    Bitboard pone = (side == WHITE) ? (pawns_promo << 8) : (pawns_promo >> 8);
    pone &= empty;
    while (pone) { int to = lsb_pop(&pone); add_pawn_promotions(ml, to - up, to, FLAG_PROMO); }

    Bitboard cl = (side == WHITE) ? ((pawns & NOT_FILE_A) << 7) & enemy
                                  : ((pawns & NOT_FILE_A) >> 9) & enemy;
    Bitboard cr = (side == WHITE) ? ((pawns & NOT_FILE_H) << 9) & enemy
                                  : ((pawns & NOT_FILE_H) >> 7) & enemy;
    Bitboard promo_dest = (side == WHITE) ? RANK_8 : RANK_1;
    while (cl) {
        int to = lsb_pop(&cl);
        int from = (side == WHITE) ? to - 7 : to + 9;
        if ((1ULL << to) & promo_dest) add_pawn_promotions(ml, from, to, FLAG_PROMO);
        else add_move(ml, encode_move(from, to, FLAG_NORMAL, 0));
    }
    while (cr) {
        int to = lsb_pop(&cr);
        int from = (side == WHITE) ? to - 9 : to + 7;
        if ((1ULL << to) & promo_dest) add_pawn_promotions(ml, from, to, FLAG_PROMO);
        else add_move(ml, encode_move(from, to, FLAG_NORMAL, 0));
    }

    if (b->ep_square != NO_SQUARE) {
        Bitboard ep_cap = pawn_attack_table[opp][b->ep_square] & pawns;
        while (ep_cap)
            add_move(ml, encode_move(lsb_pop(&ep_cap), b->ep_square, FLAG_EP, 0));
    }

    /* Knights */
    {
        Bitboard knights = b->pieces[side][KNIGHT];
        while (knights) {
            int from = lsb_pop(&knights);
            Bitboard t = knight_attack_table[from] & ~own;
            while (t) add_move(ml, encode_move(from, lsb_pop(&t), FLAG_NORMAL, 0));
        }
    }
    /* Bishops */
    {
        Bitboard bishops = b->pieces[side][BISHOP];
        while (bishops) {
            int from = lsb_pop(&bishops);
            Bitboard t = bishop_attacks(from, occ) & ~own;
            while (t) add_move(ml, encode_move(from, lsb_pop(&t), FLAG_NORMAL, 0));
        }
    }
    /* Rooks */
    {
        Bitboard rooks = b->pieces[side][ROOK];
        while (rooks) {
            int from = lsb_pop(&rooks);
            Bitboard t = rook_attacks(from, occ) & ~own;
            while (t) add_move(ml, encode_move(from, lsb_pop(&t), FLAG_NORMAL, 0));
        }
    }
    /* Queens */
    {
        Bitboard queens = b->pieces[side][QUEEN];
        while (queens) {
            int from = lsb_pop(&queens);
            Bitboard t = queen_attacks(from, occ) & ~own;
            while (t) add_move(ml, encode_move(from, lsb_pop(&t), FLAG_NORMAL, 0));
        }
    }
}

/* Captures + promotions (incl. quiet promotion pushes) + EP for quiescence */
void generate_captures(Board *b, MoveList *ml) {
    ml->count = 0;
    int side = b->side, opp = side ^ 1;
    Bitboard enemy = b->occupancy[opp];
    Bitboard occ   = b->occ_all;
    int up = (side == WHITE) ? 8 : -8;

    int king_sq = lsb(b->pieces[side][KING]);
    Bitboard km = king_attack_table[king_sq] & enemy;
    while (km) add_move(ml, encode_move(king_sq, lsb_pop(&km), FLAG_NORMAL, 0));

    Bitboard pawns = b->pieces[side][PAWN];
    Bitboard promo_rank = (side == WHITE) ? RANK_7 : RANK_2;
    Bitboard pawns_promo  = pawns & promo_rank;
    Bitboard pawns_normal = pawns & ~promo_rank;

    Bitboard cl = (side == WHITE) ? ((pawns_normal & NOT_FILE_A) << 7) & enemy
                                  : ((pawns_normal & NOT_FILE_A) >> 9) & enemy;
    Bitboard cr = (side == WHITE) ? ((pawns_normal & NOT_FILE_H) << 9) & enemy
                                  : ((pawns_normal & NOT_FILE_H) >> 7) & enemy;
    while (cl) { int to = lsb_pop(&cl); add_move(ml, encode_move((side == WHITE) ? to - 7 : to + 9, to, FLAG_NORMAL, 0)); }
    while (cr) { int to = lsb_pop(&cr); add_move(ml, encode_move((side == WHITE) ? to - 9 : to + 7, to, FLAG_NORMAL, 0)); }

    Bitboard pone = (side == WHITE) ? (pawns_promo << 8) : (pawns_promo >> 8);
    pone &= ~occ;
    while (pone) { int to = lsb_pop(&pone); add_pawn_promotions(ml, to - up, to, FLAG_PROMO); }

    Bitboard pcl = (side == WHITE) ? ((pawns_promo & NOT_FILE_A) << 7) & enemy
                                   : ((pawns_promo & NOT_FILE_A) >> 9) & enemy;
    Bitboard pcr = (side == WHITE) ? ((pawns_promo & NOT_FILE_H) << 9) & enemy
                                   : ((pawns_promo & NOT_FILE_H) >> 7) & enemy;
    while (pcl) { int to = lsb_pop(&pcl); add_pawn_promotions(ml, (side == WHITE) ? to - 7 : to + 9, to, FLAG_PROMO); }
    while (pcr) { int to = lsb_pop(&pcr); add_pawn_promotions(ml, (side == WHITE) ? to - 9 : to + 7, to, FLAG_PROMO); }

    if (b->ep_square != NO_SQUARE) {
        Bitboard ep_cap = pawn_attack_table[opp][b->ep_square] & pawns;
        while (ep_cap)
            add_move(ml, encode_move(lsb_pop(&ep_cap), b->ep_square, FLAG_EP, 0));
    }

    Bitboard knights = b->pieces[side][KNIGHT];
    while (knights) {
        int from = lsb_pop(&knights);
        Bitboard t = knight_attack_table[from] & enemy;
        while (t) add_move(ml, encode_move(from, lsb_pop(&t), FLAG_NORMAL, 0));
    }
    Bitboard bishops = b->pieces[side][BISHOP];
    while (bishops) {
        int from = lsb_pop(&bishops);
        Bitboard t = bishop_attacks(from, occ) & enemy;
        while (t) add_move(ml, encode_move(from, lsb_pop(&t), FLAG_NORMAL, 0));
    }
    Bitboard rooks = b->pieces[side][ROOK];
    while (rooks) {
        int from = lsb_pop(&rooks);
        Bitboard t = rook_attacks(from, occ) & enemy;
        while (t) add_move(ml, encode_move(from, lsb_pop(&t), FLAG_NORMAL, 0));
    }
    Bitboard queens = b->pieces[side][QUEEN];
    while (queens) {
        int from = lsb_pop(&queens);
        Bitboard t = queen_attacks(from, occ) & enemy;
        while (t) add_move(ml, encode_move(from, lsb_pop(&t), FLAG_NORMAL, 0));
    }
}

/* Fully-legal generation specialized for check evasions. Used by quiescence
 * when in check: emitting only legal evasions is cheaper than generating all
 * pseudo-legal moves and paying make/unmake + legality tests on each one,
 * because in-check positions have few legal moves and many illegal ones. */
static Bitboard compute_pinned(const Board *b, int king_sq, int side) {
    int opp = side ^ 1;
    Bitboard pinned = 0;
    Bitboard pinners =
        (rook_attacks(king_sq, 0)   & (b->pieces[opp][ROOK]   | b->pieces[opp][QUEEN])) |
        (bishop_attacks(king_sq, 0) & (b->pieces[opp][BISHOP] | b->pieces[opp][QUEEN]));
    while (pinners) {
        int pinner = lsb_pop(&pinners);
        Bitboard between = between_table[king_sq][pinner];
        Bitboard blockers = between & b->occupancy[side];
        if (popcount(blockers) == 1 && !(between & b->occupancy[opp]))
            pinned |= blockers;
    }
    return pinned;
}

void generate_evasions(Board *b, MoveList *ml) {
    ml->count = 0;
    int side = b->side, opp = side ^ 1;
    int king_sq = lsb(b->pieces[side][KING]);
    Bitboard own = b->occupancy[side];
    Bitboard enemy = b->occupancy[opp];
    Bitboard occ = b->occ_all;

    Bitboard checkers = all_attackers_to(b, king_sq, occ) & enemy;
    int num_checkers = popcount(checkers);

    /* King moves (occupancy-aware so captures of protected pieces fail) */
    {
        Bitboard km = king_attack_table[king_sq] & ~own;
        while (km) {
            int to = lsb_pop(&km);
            Bitboard captured = (1ULL << to) & enemy;
            b->occ_all &= ~(1ULL << king_sq);
            if (captured) b->occ_all &= ~captured;
            bool att = is_square_attacked(b, to, opp);
            b->occ_all = occ;
            if (!att) add_move(ml, encode_move(king_sq, to, FLAG_NORMAL, 0));
        }
    }
    if (num_checkers >= 2) return;   /* double check: king moves only */

    int checker_sq = lsb(checkers);
    Bitboard check_mask = checkers | between_table[king_sq][checker_sq];
    Bitboard pinned = compute_pinned(b, king_sq, side);

    /* Pawns */
    {
        Bitboard pawns = b->pieces[side][PAWN];
        int push_dir = (side == WHITE) ? 8 : -8;
        Bitboard promo_rank = (side == WHITE) ? RANK_7 : RANK_2;
        Bitboard start_rank = (side == WHITE) ? RANK_2 : RANK_7;
        Bitboard promo_dest = (side == WHITE) ? RANK_8 : RANK_1;

        while (pawns) {
            int from = lsb_pop(&pawns);
            Bitboard pin_ray = (pinned & (1ULL << from)) ? ray_table[king_sq][from] : ~0ULL;

            int to1 = from + push_dir;
            if (to1 >= 0 && to1 < 64 && !(occ & (1ULL << to1))) {
                if ((1ULL << to1) & check_mask & pin_ray) {
                    if ((1ULL << from) & promo_rank)
                        add_pawn_promotions(ml, from, to1, FLAG_PROMO);
                    else
                        add_move(ml, encode_move(from, to1, FLAG_NORMAL, 0));
                }
                if ((1ULL << from) & start_rank) {
                    int to2 = to1 + push_dir;
                    if (!(occ & (1ULL << to2)) && ((1ULL << to2) & check_mask & pin_ray))
                        add_move(ml, encode_move(from, to2, FLAG_NORMAL, 0));
                }
            }

            Bitboard cap_targets = pawn_attack_table[side][from] & enemy & check_mask & pin_ray;
            while (cap_targets) {
                int to = lsb_pop(&cap_targets);
                if ((1ULL << to) & promo_dest)
                    add_pawn_promotions(ml, from, to, FLAG_PROMO);
                else
                    add_move(ml, encode_move(from, to, FLAG_NORMAL, 0));
            }

            if (b->ep_square != NO_SQUARE && (pawn_attack_table[side][from] & (1ULL << b->ep_square))) {
                Move ep = encode_move(from, b->ep_square, FLAG_EP, 0);
                make_move(b, ep);
                bool legal = !is_in_check(b, side);
                unmake_move(b, ep);
                if (legal) add_move(ml, ep);
            }
        }
    }

    /* Knights */
    {
        Bitboard knights = b->pieces[side][KNIGHT] & ~pinned;
        while (knights) {
            int from = lsb_pop(&knights);
            Bitboard t = knight_attack_table[from] & ~own & check_mask;
            while (t) add_move(ml, encode_move(from, lsb_pop(&t), FLAG_NORMAL, 0));
        }
    }
    /* Bishops */
    {
        Bitboard bishops = b->pieces[side][BISHOP];
        while (bishops) {
            int from = lsb_pop(&bishops);
            Bitboard pin_ray = (pinned & (1ULL << from)) ? ray_table[king_sq][from] : ~0ULL;
            Bitboard t = bishop_attacks(from, occ) & ~own & check_mask & pin_ray;
            while (t) add_move(ml, encode_move(from, lsb_pop(&t), FLAG_NORMAL, 0));
        }
    }
    /* Rooks */
    {
        Bitboard rooks = b->pieces[side][ROOK];
        while (rooks) {
            int from = lsb_pop(&rooks);
            Bitboard pin_ray = (pinned & (1ULL << from)) ? ray_table[king_sq][from] : ~0ULL;
            Bitboard t = rook_attacks(from, occ) & ~own & check_mask & pin_ray;
            while (t) add_move(ml, encode_move(from, lsb_pop(&t), FLAG_NORMAL, 0));
        }
    }
    /* Queens */
    {
        Bitboard queens = b->pieces[side][QUEEN];
        while (queens) {
            int from = lsb_pop(&queens);
            Bitboard pin_ray = (pinned & (1ULL << from)) ? ray_table[king_sq][from] : ~0ULL;
            Bitboard t = queen_attacks(from, occ) & ~own & check_mask & pin_ray;
            while (t) add_move(ml, encode_move(from, lsb_pop(&t), FLAG_NORMAL, 0));
        }
    }
}
