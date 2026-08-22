/* evaluate.c — the handcrafted evaluation.
 *
 * All numeric parameters live in params.c (see params.h) so the texel tuner
 * can optimise them. Ideas borrowed from the classic HCE engines — full
 * attribution in CREDITS.md. */

#include "bitboard.h"
#include "params.h"

/* Non-linear king-safety curve (frozen — tuned indirectly through the
 * attack weights that feed it). */
static const int king_attack_weight[6] = { 0, 20, 20, 40, 80, 0 };
static const int king_safety_table[100] = {
      0,  0,  1,  2,  3,  5,  7,  9, 12, 15,
     18, 22, 26, 30, 35, 39, 44, 50, 56, 62,
     68, 75, 82, 85, 89, 97,105,113,122,131,
    140,150,169,180,191,202,213,225,237,248,
    260,272,283,295,307,319,330,342,354,366,
    377,389,401,412,424,436,448,459,471,517,
    556,594,631,667,702,737,771,805,838,871,
    903,935,967,998,1029,1060,1090,1121,1151,1181,
   1210,1240,1269,1298,1327,1356,1385,1414,1442,1471,
   1500,1528,1557,1585,1613,1641,1669,1697,1725,1753,
};

/* Pawn structure cache */
typedef struct {
    uint64_t key;
    Score    score;
    Bitboard passed[2];
} PawnEntry;

#define PAWN_HASH_SIZE (1 << 14)
static PawnEntry pawn_hash[PAWN_HASH_SIZE];

void eval_clear_pawn_hash(void) { memset(pawn_hash, 0, sizeof(pawn_hash)); }

static inline Score mul_sign(Score s, int c) { return c == WHITE ? s : S(-s.mg, -s.eg); }

static inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static int chebyshev(int sq1, int sq2) {
    int rd = abs(rank_of(sq1) - rank_of(sq2));
    int fd = abs(file_of(sq1) - file_of(sq2));
    return rd > fd ? rd : fd;
}

static Score evaluate_pawn_structure(const Board *b, Bitboard passed_out[2]) {
    Score s = S(0, 0);
    passed_out[WHITE] = 0;
    passed_out[BLACK] = 0;

    for (int c = WHITE; c <= BLACK; c++) {
        int opp = c ^ 1;
        Bitboard pawns = b->pieces[c][PAWN];
        Bitboard opp_pawns = b->pieces[opp][PAWN];

        while (pawns) {
            int sq = lsb_pop(&pawns);
            int f = file_of(sq);
            Bitboard file_bb = file_bitboard[f];
            Bitboard adj = adjacent_files[f];

            /* Passed pawn */
            if (!(opp_pawns & passed_pawn_mask[c][sq])) passed_out[c] |= 1ULL << sq;

            /* Doubled */
            Bitboard behind = (c == WHITE) ? forward_file_mask[BLACK][sq] : forward_file_mask[WHITE][sq];
            if (b->pieces[c][PAWN] & behind & file_bb)
                s = score_sub(s, mul_sign(S(DOUBLED[MG], DOUBLED[EG]), c));

            /* Isolated */
            if (!(b->pieces[c][PAWN] & adj)) {
                s = score_sub(s, mul_sign(S(ISOLATED[MG], ISOLATED[EG]), c));
            }
            /* Connected / phalanx */
            else {
                int rel = relative_rank(c, sq);
                Bitboard support = pawn_attack_table[opp][sq] & b->pieces[c][PAWN] & adj;
                if (support) s = score_add(s, mul_sign(S(CONNECTED[MG][rel], CONNECTED[EG][rel]), c));
            }

            /* Backward */
            if (b->pieces[c][PAWN] & adj) {
                int stop = (c == WHITE) ? sq + 8 : sq - 8;
                if (stop >= 0 && stop < 64) {
                    bool attacked_by_enemy_pawn = (pawn_attack_table[c][stop] & opp_pawns) != 0;
                    Bitboard defenders = pawn_attack_table[opp][stop] & b->pieces[c][PAWN];
                    Bitboard support_here = pawn_attack_table[opp][sq] & b->pieces[c][PAWN];
                    if (attacked_by_enemy_pawn && !defenders && !support_here)
                        s = score_sub(s, mul_sign(S(BACKWARD[MG], BACKWARD[EG]), c));
                }
            }
        }
    }
    return s;
}

uint64_t pawn_structure_key(const Board *b) {
    return splitmix64(b->pieces[WHITE][PAWN]) ^ splitmix64(b->pieces[BLACK][PAWN] ^ 0xABCDEFULL);
}

static const PawnEntry *get_pawn_entry(const Board *b) {
    uint64_t key = pawn_structure_key(b);
    uint64_t idx = key & (PAWN_HASH_SIZE - 1);
    PawnEntry *e = &pawn_hash[idx];
    if (e->key != key) {
        Bitboard passed[2];
        e->key   = key;
        e->score = evaluate_pawn_structure(b, passed);
        e->passed[WHITE] = passed[WHITE];
        e->passed[BLACK] = passed[BLACK];
    }
    return e;
}

/* Material-based drawishness: returns a scale factor in [0..64] applied to
 * the endgame component before tapering (Shredder/Komodo tradition). */
static int endgame_scale(const Board *b) {
    int scale = 64;

    Bitboard wp = b->pieces[WHITE][PAWN], bp = b->pieces[BLACK][PAWN];
    int npc_w = popcount(b->occupancy[WHITE] & ~wp & ~b->pieces[WHITE][KING]);
    int npc_b = popcount(b->occupancy[BLACK] & ~bp & ~b->pieces[BLACK][KING]);

    /* No queens on the board for the classic drawish configurations */
    if (!b->pieces[WHITE][QUEEN] && !b->pieces[BLACK][QUEEN]) {
        /* Opposite-coloured bishops, no other pieces */
        if (popcount(b->pieces[WHITE][BISHOP]) == 1 && popcount(b->pieces[BLACK][BISHOP]) == 1 &&
            npc_w == 1 && npc_b == 1) {
            int wsq = lsb(b->pieces[WHITE][BISHOP]);
            int bsq = lsb(b->pieces[BLACK][BISHOP]);
            if (((rank_of(wsq) + file_of(wsq)) & 1) != ((rank_of(bsq) + file_of(bsq)) & 1)) {
                int pd = popcount(wp) - popcount(bp);
                scale = (pd >= -2 && pd <= 2) ? 40 : 52;
            }
        }
        /* Rook endings where the stronger side is only one pawn up */
        if (popcount(b->pieces[WHITE][ROOK]) == 1 && popcount(b->pieces[BLACK][ROOK]) == 1 &&
            npc_w == 1 && npc_b == 1) {
            if (abs((int)(popcount(wp) - popcount(bp))) == 1) scale = 56;
        }
        /* Pawnless endings need more than a rook's worth of material to win */
        if (!wp && !bp) {
            int mat_w = 0, mat_b = 0;
            for (int p = KNIGHT; p <= QUEEN; p++) {
                mat_w += popcount(b->pieces[WHITE][p]) * MATERIAL_EG[p];
                mat_b += popcount(b->pieces[BLACK][p]) * MATERIAL_EG[p];
            }
            if (abs(mat_w - mat_b) < 480) scale = 16;
        }
    }

    /* KBP vs K with a rook pawn and a bishop of the wrong colour: fortress */
    {
        for (int c = WHITE; c <= BLACK; c++) {
            int opp = c ^ 1;
            Bitboard cp = b->pieces[c][PAWN];
            Bitboard opp_all = b->occupancy[opp];
            /* only K+B+P vs K remains on the strong side */
            if (popcount(b->pieces[c][BISHOP]) == 1 && !b->pieces[c][KNIGHT] &&
                !b->pieces[c][ROOK] && !b->pieces[c][QUEEN] &&
                popcount(cp) == 1 && popcount(opp_all) == 1) {
                int pawn_sq = lsb(cp);
                int f = file_of(pawn_sq);
                if (f == 0 || f == 7) {
                    int bsq = lsb(b->pieces[c][BISHOP]);
                    int promo_sq = (c == WHITE) ? (f | 56) : f;   /* a8 / h8 or a1 / h1 */
                    int bishop_color = (rank_of(bsq) + file_of(bsq)) & 1;
                    int promo_color  = (rank_of(promo_sq) + file_of(promo_sq)) & 1;
                    if (bishop_color != promo_color) return 4;
                }
            }
        }
    }
    return scale;
}

int evaluate(Board *b) {
    int phase = b->game_phase > TOTAL_PHASE ? TOTAL_PHASE : b->game_phase;

    int wksq = lsb(b->pieces[WHITE][KING]);
    int bksq = lsb(b->pieces[BLACK][KING]);

    const PawnEntry *pe = get_pawn_entry(b);
    Score total = pe->score;

    /* Material + PST from incremental accumulators */
    total.mg += b->psqt[WHITE][MG] - b->psqt[BLACK][MG];
    total.eg += b->psqt[WHITE][EG] - b->psqt[BLACK][EG];

    Bitboard wpawn_attacks = ((b->pieces[WHITE][PAWN] & NOT_FILE_H) << 9) |
                             ((b->pieces[WHITE][PAWN] & NOT_FILE_A) << 7);
    Bitboard bpawn_attacks = ((b->pieces[BLACK][PAWN] & NOT_FILE_A) >> 7) |
                             ((b->pieces[BLACK][PAWN] & NOT_FILE_H) >> 9);

    Bitboard king_zone[2];
    {
        Bitboard wz = king_attack_table[wksq] | (1ULL << wksq);
        king_zone[WHITE] = wz | (wz << 8);
        Bitboard bz = king_attack_table[bksq] | (1ULL << bksq);
        king_zone[BLACK] = bz | (bz >> 8);
    }

    int attack_units[2] = {0, 0};
    int attacker_count[2] = {0, 0};

    Bitboard occ = b->occ_all;

    for (int c = WHITE; c <= BLACK; c++) {
        int opp = c ^ 1;
        int sign = (c == WHITE) ? 1 : -1;
        Bitboard own_pawn_att = (c == WHITE) ? wpawn_attacks : bpawn_attacks;
        Bitboard enemy_pawn_att = (c == WHITE) ? bpawn_attacks : wpawn_attacks;
        Bitboard opp_kz = king_zone[opp];

        /* Knights */
        {
            Bitboard knights = b->pieces[c][KNIGHT];
            while (knights) {
                int sq = lsb_pop(&knights);
                int rel = relative_rank(c, sq);
                Bitboard att = knight_attack_table[sq];
                int mob = popcount(att & ~b->occupancy[c] & ~enemy_pawn_att);
                total.mg += sign * KNIGHT_MOB[MG][mob];
                total.eg += sign * KNIGHT_MOB[EG][mob];

                if (rel >= 4 && !(passed_pawn_mask[opp][sq] & b->pieces[opp][PAWN])) {
                    int bonus = KNIGHT_OUTPOST[MG] + ((own_pawn_att & (1ULL << sq)) ? 10 : 0);
                    int bonus_eg = KNIGHT_OUTPOST[EG] + ((own_pawn_att & (1ULL << sq)) ? 4 : 0);
                    total.mg += sign * bonus;
                    total.eg += sign * bonus_eg;
                }

                if (att & opp_kz) {
                    attack_units[opp] += king_attack_weight[KNIGHT] / 10;
                    attacker_count[opp]++;
                }

                if (att & b->pieces[opp][QUEEN]) {
                    total.mg += sign * THREAT_Q_BY_MINOR[MG];
                    total.eg += sign * THREAT_Q_BY_MINOR[EG];
                }
            }
        }

        /* Bishops */
        {
            Bitboard bishops = b->pieces[c][BISHOP];
            int nb = popcount(bishops);
            while (bishops) {
                int sq = lsb_pop(&bishops);
                Bitboard att = bishop_attacks(sq, occ);
                int mob = popcount(att & ~b->occupancy[c] & ~enemy_pawn_att);
                total.mg += sign * BISHOP_MOB[MG][mob];
                total.eg += sign * BISHOP_MOB[EG][mob];

                int rel = relative_rank(c, sq);
                if (rel >= 4 && !(passed_pawn_mask[opp][sq] & b->pieces[opp][PAWN])) {
                    int bonus = BISHOP_OUTPOST[MG] + ((own_pawn_att & (1ULL << sq)) ? 8 : 0);
                    int bonus_eg = BISHOP_OUTPOST[EG] + ((own_pawn_att & (1ULL << sq)) ? 3 : 0);
                    total.mg += sign * bonus;
                    total.eg += sign * bonus_eg;
                }

                /* Bad bishop: own pawns fixed on the bishop's colour complex */
                int sq_color = (rank_of(sq) + file_of(sq)) & 1;
                int same_color_pawns = popcount(b->pieces[c][PAWN] &
                    (sq_color ? 0xAA55AA55AA55AA55ULL : 0x55AA55AA55AA55AAULL));
                total.mg += sign * BAD_BISHOP[MG] * same_color_pawns;
                total.eg += sign * BAD_BISHOP[EG] * same_color_pawns;

                if (att & opp_kz) {
                    attack_units[opp] += king_attack_weight[BISHOP] / 10;
                    attacker_count[opp]++;
                }

                if (att & b->pieces[opp][QUEEN]) {
                    total.mg += sign * THREAT_Q_BY_MINOR[MG];
                    total.eg += sign * THREAT_Q_BY_MINOR[EG];
                }
            }
            if (nb >= 2) { total.mg += sign * BISHOP_PAIR[MG]; total.eg += sign * BISHOP_PAIR[EG]; }
        }

        /* Rooks */
        {
            Bitboard rooks = b->pieces[c][ROOK];
            while (rooks) {
                int sq = lsb_pop(&rooks);
                Bitboard att = rook_attacks(sq, occ);
                int mob = popcount(att & ~b->occupancy[c]);
                total.mg += sign * ROOK_MOB[MG][mob];
                total.eg += sign * ROOK_MOB[EG][mob];

                int f = file_of(sq);
                Bitboard fb = file_bitboard[f];
                bool no_own_pawn = !(b->pieces[c][PAWN] & fb);
                bool no_opp_pawn = !(b->pieces[opp][PAWN] & fb);
                if (no_own_pawn && no_opp_pawn) { total.mg += sign * ROOK_OPEN[MG]; total.eg += sign * ROOK_OPEN[EG]; }
                else if (no_own_pawn)           { total.mg += sign * ROOK_SEMI[MG]; total.eg += sign * ROOK_SEMI[EG]; }

                if (popcount(b->pieces[c][ROOK] & fb) >= 2) {
                    total.mg += sign * ROOK_DOUBLED[MG]; total.eg += sign * ROOK_DOUBLED[EG];
                }

                if (att & opp_kz) {
                    attack_units[opp] += king_attack_weight[ROOK] / 10;
                    attacker_count[opp]++;
                }

                if (att & b->pieces[opp][QUEEN]) {
                    total.mg += sign * THREAT_Q_BY_ROOK[MG];
                    total.eg += sign * THREAT_Q_BY_ROOK[EG];
                }
            }
        }

        /* Queens */
        {
            Bitboard queens = b->pieces[c][QUEEN];
            while (queens) {
                int sq = lsb_pop(&queens);
                Bitboard att = rook_attacks(sq, occ) | bishop_attacks(sq, occ);
                int mob = popcount(att & ~b->occupancy[c] & ~enemy_pawn_att);
                total.mg += sign * QUEEN_MOB[MG][mob];
                total.eg += sign * QUEEN_MOB[EG][mob];

                if (att & opp_kz) {
                    attack_units[opp] += king_attack_weight[QUEEN] / 10;
                    attacker_count[opp]++;
                }

                int rel = relative_rank(c, sq);
                int own_dev_pawns = popcount(b->pieces[c][PAWN] &
                    ((c == WHITE) ? (RANK_2 | (RANK_2 << 8)) : (RANK_7 | (RANK_7 >> 8))));
                if (phase >= 20 && rel >= 3 && own_dev_pawns >= 5) {
                    total.mg += sign * QUEEN_EARLY[MG];
                    total.eg += sign * QUEEN_EARLY[EG];
                }
            }
        }

        /* Threats by pawns on pieces */
        {
            Bitboard threatened = enemy_pawn_att & b->occupancy[c];
            Bitboard minors = b->pieces[c][KNIGHT] | b->pieces[c][BISHOP];
            Bitboard rookq  = b->pieces[c][ROOK] | b->pieces[c][QUEEN];
            int tm = popcount(threatened & minors);
            int tr = popcount(threatened & rookq);
            total.mg += sign * tm * THREAT_PAWN_MINOR[MG];
            total.eg += sign * tm * THREAT_PAWN_MINOR[EG];
            total.mg += sign * tr * THREAT_PAWN_ROOKQ[MG];
            total.eg += sign * tr * THREAT_PAWN_ROOKQ[EG];
        }

        /* Space */
        {
            Bitboard area = (c == WHITE)
                ? (RANK_2 | RANK_3 | RANK_4 | (RANK_2 << 24) | (RANK_2 << 16))
                : (RANK_7 | RANK_6 | RANK_5 | (RANK_7 >> 24) | (RANK_7 >> 16));
            area &= ~b->pieces[c][PAWN];
            int sp = popcount(area & own_pawn_att);
            int factor = popcount(b->occupancy[c]) - 2;
            if (factor > 10) factor = 10;
            if (factor > 0) {
                total.mg += sign * sp * SPACE[MG] * factor / 10;
                total.eg += sign * sp * SPACE[EG] * factor / 10;
            }
        }

        /* Pawn shield / storm for own king */
        {
            int ksq = (c == WHITE) ? wksq : bksq;
            int kf = file_of(ksq);
            int kr = rank_of(ksq);
            bool castled = (c == WHITE) ? (kr == 0) : (kr == 7);
            Bitboard shield_zone = (c == WHITE) ? (RANK_2 | RANK_3) : (RANK_7 | RANK_6);
            int lo = kf > 0 ? kf - 1 : 0;
            int hi = kf < 7 ? kf + 1 : 7;
            if (castled && phase >= 8) {
                for (int f = lo; f <= hi; f++) {
                    Bitboard zone = shield_zone & file_bitboard[f];
                    if (!(b->pieces[c][PAWN] & zone)) {
                        total.mg += sign * SHIELD_MISSING[MG];
                        total.eg += sign * SHIELD_MISSING[EG];
                    }
                }
            }
            Bitboard storm_zone = (c == WHITE)
                ? ((RANK_2 << 16) | (RANK_2 << 24) | (RANK_2 << 32))
                : ((RANK_7 >> 16) | (RANK_7 >> 24) | (RANK_7 >> 32));
            Bitboard storm_files = 0;
            for (int f = lo; f <= hi; f++) storm_files |= file_bitboard[f];
            int storm_dist = popcount(b->pieces[opp][PAWN] & storm_zone & storm_files);
            int sidx = storm_dist < 4 ? storm_dist : 4;
            total.mg += sign * STORM[MG][sidx];
            total.eg += sign * STORM[EG][sidx];
        }
    }

    /* Passed pawn evaluation */
    for (int c = WHITE; c <= BLACK; c++) {
        int sign = (c == WHITE) ? 1 : -1;
        int own_ksq = (c == WHITE) ? wksq : bksq;
        int opp_ksq = (c == WHITE) ? bksq : wksq;
        Bitboard passers = pe->passed[c];
        while (passers) {
            int sq = lsb_pop(&passers);
            int rel = relative_rank(c, sq);
            total.mg += sign * PASSED_BASE[MG][rel];
            total.eg += sign * PASSED_BASE[EG][rel];

            Bitboard path = (c == WHITE) ? forward_file_mask[WHITE][sq] : forward_file_mask[BLACK][sq];
            if (!(b->occ_all & path)) {
                total.mg += sign * PASSED_FREE[MG];
                total.eg += sign * PASSED_FREE[EG];
            }
            if (pawn_attack_table[c ^ 1][sq] & b->pieces[c][PAWN]) {
                total.mg += sign * PASSED_DEFENDED[MG];
                total.eg += sign * PASSED_DEFENDED[EG];
            }
            /* Kings race to the passer (endgame weighted) */
            int d_own = chebyshev(own_ksq, sq);
            int d_opp = chebyshev(opp_ksq, sq);
            int kd = PASSER_KD[0][d_own < 7 ? d_own : 7] + PASSER_KD[1][d_opp < 7 ? d_opp : 7];
            total.eg += sign * kd * (TOTAL_PHASE - phase) / TOTAL_PHASE;

            Bitboard behind = (c == WHITE) ? forward_file_mask[BLACK][sq] : forward_file_mask[WHITE][sq];
            if (b->pieces[c][ROOK] & behind) {
                total.mg += sign * ROOK_BEHIND[MG];
                total.eg += sign * ROOK_BEHIND[EG];
            }
        }
    }

    /* King-zone attack score with non-linear safety table */
    for (int c = WHITE; c <= BLACK; c++) {
        if (attacker_count[c] < 2) continue;
        int units = attack_units[c];
        units -= 2 * (TOTAL_PHASE - phase);
        if (units < 0) units = 0;
        if (units > 99) units = 99;
        int danger = king_safety_table[units] * phase / TOTAL_PHASE;
        total.mg += (c == WHITE) ? -danger : danger;
    }

    int scale = endgame_scale(b);
    int mg = total.mg;
    int eg = total.eg * scale / 64;
    int score = (mg * phase + eg * (TOTAL_PHASE - phase)) / TOTAL_PHASE;

    /* Tempo: from the point of view of the side to move */
    score += TEMPO[MG] * phase / TOTAL_PHASE + TEMPO[EG] * (TOTAL_PHASE - phase) / TOTAL_PHASE;

    return (b->side == WHITE) ? score : -score;
}

bool is_insufficient_material(const Board *b) {
    if (b->occ_all == (b->pieces[WHITE][KING] | b->pieces[BLACK][KING])) return true;
    if (popcount(b->occ_all) == 3 &&
        (b->pieces[WHITE][BISHOP] || b->pieces[BLACK][BISHOP] ||
         b->pieces[WHITE][KNIGHT] || b->pieces[BLACK][KNIGHT])) return true;
    if (popcount(b->occ_all) == 4 &&
        popcount(b->pieces[WHITE][BISHOP]) == 1 &&
        popcount(b->pieces[BLACK][BISHOP]) == 1) {
        int wsq = lsb(b->pieces[WHITE][BISHOP]);
        int bsq = lsb(b->pieces[BLACK][BISHOP]);
        if (((rank_of(wsq) + file_of(wsq)) & 1) == ((rank_of(bsq) + file_of(bsq)) & 1)) return true;
    }
    return false;
}
