/* movegen.c — pseudo-legal move generation and move validation.
 *
 * Generated moves are pseudo-legal; callers filter them with
 * move_is_legal() before make_move(), using the checkers/pinned sets kept
 * up to date by the board. Castling is generated fully legal (its attack
 * tests are part of the rules). move_is_pseudo_legal() validates moves from
 * untrusted sources such as the transposition table and killer slots. */

#include "bitboard.h"

static void add_move(MoveList *ml, Move m) { ml->moves[ml->count++] = m; }

static void add_pawn_promotions(MoveList *ml, int from, int to, int flags) {
    add_move(ml, encode_move(from, to, flags, PROMO_QUEEN));
    add_move(ml, encode_move(from, to, flags, PROMO_KNIGHT));
    add_move(ml, encode_move(from, to, flags, PROMO_ROOK));
    add_move(ml, encode_move(from, to, flags, PROMO_BISHOP));
}

/* Squares from a to z inclusive, both on the same rank. */
static Bitboard rank_span(int a, int z) {
    int lo = a < z ? a : z, hi = a > z ? a : z;
    Bitboard span = 0;
    for (int sq = lo; sq <= hi; sq++) span |= 1ULL << sq;
    return span;
}

static bool attacked_with_occ(const Board *b, int sq, int by, Bitboard occ) {
    return (pawn_attack_table[by ^ 1][sq] & b->pieces[by][PAWN]) ||
           (knight_attack_table[sq] & b->pieces[by][KNIGHT]) ||
           (king_attack_table[sq] & b->pieces[by][KING]) ||
           (rook_attacks(sq, occ) & (b->pieces[by][ROOK] | b->pieces[by][QUEEN]) & occ) ||
           (bishop_attacks(sq, occ) & (b->pieces[by][BISHOP] | b->pieces[by][QUEEN]) & occ);
}

/* Legal castling moves, encoded king-from to king-destination (g/c file).
 * Works for Chess960: the king and rook paths only have to be free of other
 * pieces, and the king may not pass through or land on an attacked square. */
static void gen_castling(const Board *b, MoveList *ml) {
    int side = b->side, opp = side ^ 1;
    if (!b->pieces[side][KING] || b->checkers) return;
    int king = lsb(b->pieces[side][KING]);
    if (rank_of(king) != 7 * side) return;
    for (int i = 2 * side; i < 2 * side + 2; i++) {
        if (!(b->castling_rights & (1 << i))) continue;
        bool queenside = i % 2;
        int rook = b->castle_rook[i];
        int king_to = 56 * side + (queenside ? 2 : 6);
        int rook_to = 56 * side + (queenside ? 3 : 5);
        if (rook >= 64 || !(b->pieces[side][ROOK] & (1ULL << rook))) continue;
        Bitboard origins = (1ULL << king) | (1ULL << rook);
        if ((rank_span(king, king_to) | rank_span(rook, rook_to)) & (b->occ_all & ~origins)) continue;

        Bitboard occ = b->occ_all & ~(1ULL << king);
        bool safe = true;
        int step = king_to > king ? 1 : -1;
        for (int sq = king; sq != king_to && safe;) {
            sq += step;
            safe = !attacked_with_occ(b, sq, opp, occ);
        }
        /* Moving the rook can uncover an attack on a stationary king. */
        if (safe && attacked_with_occ(b, king_to, opp, (b->occ_all & ~origins) | (1ULL << rook_to)))
            safe = false;
        if (safe) add_move(ml, encode_move(king, king_to, FLAG_CASTLE, 0));
    }
}

void generate_moves(Board *b, MoveList *ml) {
    ml->count = 0;
    int side = b->side, opp = side ^ 1;
    Bitboard own   = b->occupancy[side];
    Bitboard enemy = b->occupancy[opp];
    Bitboard occ   = b->occ_all;
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
    while (cl) {
        int to = lsb_pop(&cl);
        add_move(ml, encode_move((side == WHITE) ? to - 7 : to + 9, to, FLAG_NORMAL, 0));
    }
    while (cr) {
        int to = lsb_pop(&cr);
        add_move(ml, encode_move((side == WHITE) ? to - 9 : to + 7, to, FLAG_NORMAL, 0));
    }

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

/* Quiet moves: everything generate_captures() leaves out. */
void generate_quiets(Board *b, MoveList *ml) {
    ml->count = 0;
    int side = b->side;
    Bitboard empty = ~b->occ_all;
    Bitboard occ = b->occ_all;
    int up = (side == WHITE) ? 8 : -8;

    int king_sq = lsb(b->pieces[side][KING]);
    Bitboard km = king_attack_table[king_sq] & empty;
    while (km) add_move(ml, encode_move(king_sq, lsb_pop(&km), FLAG_NORMAL, 0));
    gen_castling(b, ml);

    Bitboard pawns = b->pieces[side][PAWN] & ~((side == WHITE) ? RANK_7 : RANK_2);
    Bitboard one = ((side == WHITE) ? (pawns << 8) : (pawns >> 8)) & empty;
    Bitboard two = ((side == WHITE) ? ((one & RANK_3) << 8) : ((one & RANK_6) >> 8)) & empty;
    while (one) { int to = lsb_pop(&one); add_move(ml, encode_move(to - up, to, FLAG_NORMAL, 0)); }
    while (two) { int to = lsb_pop(&two); add_move(ml, encode_move(to - 2 * up, to, FLAG_NORMAL, 0)); }

    for (int pt = KNIGHT; pt <= QUEEN; pt++) {
        Bitboard pieces = b->pieces[side][pt];
        while (pieces) {
            int from = lsb_pop(&pieces);
            Bitboard t = pt == KNIGHT ? knight_attack_table[from]
                       : pt == BISHOP ? bishop_attacks(from, occ)
                       : pt == ROOK   ? rook_attacks(from, occ)
                                      : queen_attacks(from, occ);
            t &= empty;
            while (t) add_move(ml, encode_move(from, lsb_pop(&t), FLAG_NORMAL, 0));
        }
    }
}

bool move_is_noisy(const Board *b, Move m) {
    if (move_is_castle(m)) return false;
    return move_is_promo(m) || move_is_ep(m) || (b->occ_all & (1ULL << move_to(m)));
}

static Bitboard piece_attacks(int pt, int side, int sq, Bitboard occ) {
    switch (pt) {
        case PAWN:   return pawn_attack_table[side][sq];
        case KNIGHT: return knight_attack_table[sq];
        case BISHOP: return bishop_attacks(sq, occ);
        case ROOK:   return rook_attacks(sq, occ);
        case QUEEN:  return queen_attacks(sq, occ);
        default:     return king_attack_table[sq];
    }
}

/* Validates a move from an untrusted source (TT, killer). */
bool move_is_pseudo_legal(const Board *b, Move m) {
    if (m == NO_MOVE) return false;
    int side = b->side, from = move_from(m), to = move_to(m), flags = move_flags(m);
    Bitboard from_bb = 1ULL << from, to_bb = 1ULL << to;
    if (!(b->occupancy[side] & from_bb)) return false;
    int pt = b->piece_on[from] % 6;

    if (flags == FLAG_CASTLE) {
        if (pt != KING) return false;
        MoveList ml; ml.count = 0;
        gen_castling(b, &ml);
        for (int i = 0; i < ml.count; i++) if (ml.moves[i] == m) return true;
        return false;
    }
    if (b->occupancy[side] & to_bb) return false;
    if (move_promo(m) && flags != FLAG_PROMO) return false;

    if (pt != PAWN) {
        if (flags != FLAG_NORMAL) return false;
        return (piece_attacks(pt, side, from, b->occ_all) & to_bb) != 0;
    }

    Bitboard last = (side == WHITE) ? RANK_8 : RANK_1;
    if (flags == FLAG_EP)
        return to == b->ep_square && (pawn_attack_table[side][from] & to_bb);
    if ((flags == FLAG_PROMO) != ((to_bb & last) != 0)) return false;

    if (pawn_attack_table[side][from] & to_bb & b->occupancy[side ^ 1]) return true;
    int up = (side == WHITE) ? 8 : -8;
    if (b->occ_all & to_bb) return false;
    if (to == from + up) return true;
    Bitboard start = (side == WHITE) ? RANK_2 : RANK_7;
    return to == from + 2 * up && (from_bb & start) && !(b->occ_all & (1ULL << (from + up)));
}

/* Legality of a pseudo-legal move, without making it. */
bool move_is_legal(const Board *b, Move m) {
    int side = b->side, them = side ^ 1;
    int from = move_from(m), to = move_to(m);
    int ksq = lsb(b->pieces[side][KING]);
    if (move_is_castle(m)) return true;   /* generated or validated fully legal */

    if (from == ksq) {
        return !attacked_with_occ(b, to, them, b->occ_all ^ (1ULL << from));
    }
    if (move_is_ep(m)) {
        int cap = (side == WHITE) ? to - 8 : to + 8;
        Bitboard occ = (b->occ_all ^ (1ULL << from) ^ (1ULL << cap)) | (1ULL << to);
        Bitboard rq = b->pieces[them][ROOK] | b->pieces[them][QUEEN];
        Bitboard bq = b->pieces[them][BISHOP] | b->pieces[them][QUEEN];
        Bitboard others = b->checkers & ~(1ULL << cap);
        if (others & (b->pieces[them][KNIGHT] | b->pieces[them][PAWN])) return false;
        return !(rook_attacks(ksq, occ) & rq) && !(bishop_attacks(ksq, occ) & bq);
    }
    if (b->checkers) {
        if (b->checkers & (b->checkers - 1)) return false;
        int c = lsb(b->checkers);
        if (!((b->checkers | between_table[ksq][c]) & (1ULL << to))) return false;
    }
    if ((b->pinned & (1ULL << from)) && !(ray_table[ksq][from] & (1ULL << to))) return false;
    return true;
}

/* Does the move check the opponent's king? Exact for every move type. */
bool move_gives_check(const Board *b, Move m) {
    int side = b->side, them = side ^ 1;
    int from = move_from(m), to = move_to(m);
    int eksq = lsb(b->pieces[them][KING]);
    Bitboard occ = (b->occ_all ^ (1ULL << from)) | (1ULL << to);
    int pt = b->piece_on[from] % 6;

    if (move_is_castle(m)) {
        int i = 2 * side + (file_of(to) == 2);
        int rf = b->castle_rook[i], rt = 8 * rank_of(to) + (file_of(to) == 6 ? 5 : 3);
        occ = (b->occ_all & ~((1ULL << from) | (1ULL << rf))) | (1ULL << to) | (1ULL << rt);
        return (rook_attacks(rt, occ) & (1ULL << eksq)) != 0;
    }
    if (move_is_ep(m)) occ ^= 1ULL << ((side == WHITE) ? to - 8 : to + 8);
    if (move_is_promo(m)) {
        static const int promo_piece[] = {KNIGHT, BISHOP, ROOK, QUEEN};
        pt = promo_piece[move_promo(m)];
    }
    if (piece_attacks(pt, side, to, occ) & (1ULL << eksq)) return true;
    /* Discovered check by a slider behind the moved piece. */
    Bitboard rq = (b->pieces[side][ROOK] | b->pieces[side][QUEEN]) & ~(1ULL << from);
    Bitboard bq = (b->pieces[side][BISHOP] | b->pieces[side][QUEEN]) & ~(1ULL << from);
    return ((rook_attacks(eksq, occ) & rq) | (bishop_attacks(eksq, occ) & bq)) != 0;
}
