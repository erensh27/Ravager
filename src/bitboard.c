/* bitboard.c — precomputed attack tables.
 * Sliding attacks use BMI2 PEXT when available, otherwise plain magics-free
 * subset enumeration is impossible at runtime, so we fall back to classical
 * ray scans wrapped in a branchless loop (still fast enough, and correct). */

#include "bitboard.h"

Bitboard knight_attack_table[64];
Bitboard king_attack_table[64];
Bitboard pawn_attack_table[2][64];
Bitboard passed_pawn_mask[2][64];
Bitboard isolated_mask[64];
Bitboard forward_file_mask[2][64];
Bitboard between_table[64][64];
Bitboard ray_table[64][64];
Bitboard file_bitboard[8];
Bitboard adjacent_files[8];

#if defined(__BMI2__) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define USE_PEXT 1
static Bitboard rook_attack_table[64][4096];
static Bitboard bishop_attack_table[64][512];
static Bitboard rook_masks[64], bishop_masks[64];
#endif

static Bitboard sliding_rook(int sq, Bitboard occ) {
    Bitboard a = 0;
    int r = sq >> 3, f = sq & 7;
    for (int rr = r+1; rr <= 7; rr++) { a |= 1ULL << (rr*8+f); if (occ & (1ULL << (rr*8+f))) break; }
    for (int rr = r-1; rr >= 0; rr--) { a |= 1ULL << (rr*8+f); if (occ & (1ULL << (rr*8+f))) break; }
    for (int ff = f+1; ff <= 7; ff++) { a |= 1ULL << (r*8+ff); if (occ & (1ULL << (r*8+ff))) break; }
    for (int ff = f-1; ff >= 0; ff--) { a |= 1ULL << (r*8+ff); if (occ & (1ULL << (r*8+ff))) break; }
    return a;
}
static Bitboard sliding_bishop(int sq, Bitboard occ) {
    Bitboard a = 0;
    int r = sq >> 3, f = sq & 7;
    for (int rr = r+1, ff = f+1; rr <= 7 && ff <= 7; rr++, ff++) { a |= 1ULL << (rr*8+ff); if (occ & (1ULL << (rr*8+ff))) break; }
    for (int rr = r+1, ff = f-1; rr <= 7 && ff >= 0; rr++, ff--) { a |= 1ULL << (rr*8+ff); if (occ & (1ULL << (rr*8+ff))) break; }
    for (int rr = r-1, ff = f+1; rr >= 0 && ff <= 7; rr--, ff++) { a |= 1ULL << (rr*8+ff); if (occ & (1ULL << (rr*8+ff))) break; }
    for (int rr = r-1, ff = f-1; rr >= 0 && ff >= 0; rr--, ff--) { a |= 1ULL << (rr*8+ff); if (occ & (1ULL << (rr*8+ff))) break; }
    return a;
}

Bitboard rook_attacks(int sq, Bitboard occ) {
#ifdef USE_PEXT
    return rook_attack_table[sq][_pext_u64(occ, rook_masks[sq])];
#else
    return sliding_rook(sq, occ);
#endif
}
Bitboard bishop_attacks(int sq, Bitboard occ) {
#ifdef USE_PEXT
    return bishop_attack_table[sq][_pext_u64(occ, bishop_masks[sq])];
#else
    return sliding_bishop(sq, occ);
#endif
}
Bitboard queen_attacks(int sq, Bitboard occ) {
    return rook_attacks(sq, occ) | bishop_attacks(sq, occ);
}

static Bitboard compute_rook_mask(int sq) {
    Bitboard m = 0; int r = sq >> 3, f = sq & 7;
    for (int rr = r+1; rr <= 6; rr++) m |= 1ULL << (rr*8+f);
    for (int rr = r-1; rr >= 1; rr--) m |= 1ULL << (rr*8+f);
    for (int ff = f+1; ff <= 6; ff++) m |= 1ULL << (r*8+ff);
    for (int ff = f-1; ff >= 1; ff--) m |= 1ULL << (r*8+ff);
    return m;
}
static Bitboard compute_bishop_mask(int sq) {
    Bitboard m = 0; int r = sq >> 3, f = sq & 7;
    for (int rr = r+1, ff = f+1; rr <= 6 && ff <= 6; rr++, ff++) m |= 1ULL << (rr*8+ff);
    for (int rr = r+1, ff = f-1; rr <= 6 && ff >= 1; rr++, ff--) m |= 1ULL << (rr*8+ff);
    for (int rr = r-1, ff = f+1; rr >= 1 && ff <= 6; rr--, ff++) m |= 1ULL << (rr*8+ff);
    for (int rr = r-1, ff = f-1; rr >= 1 && ff >= 1; rr--, ff--) m |= 1ULL << (rr*8+ff);
    return m;
}

void init_bitboards(void) {
    for (int sq = 0; sq < 64; sq++) {
        int r = rank_of(sq), f = file_of(sq);
        Bitboard b = 1ULL << sq;

        Bitboard n = 0;
        if (r+2<=7 && f+1<=7) n |= b << 17;
        if (r+2<=7 && f-1>=0) n |= b << 15;
        if (r-2>=0 && f+1<=7) n |= b >> 15;
        if (r-2>=0 && f-1>=0) n |= b >> 17;
        if (r+1<=7 && f+2<=7) n |= b << 10;
        if (r+1<=7 && f-2>=0) n |= b << 6;
        if (r-1>=0 && f+2<=7) n |= b >> 6;
        if (r-1>=0 && f-2>=0) n |= b >> 10;
        knight_attack_table[sq] = n;

        Bitboard k = 0;
        if (r+1<=7) k |= b << 8;
        if (r-1>=0) k |= b >> 8;
        if (f+1<=7) k |= b << 1;
        if (f-1>=0) k |= b >> 1;
        if (r+1<=7 && f+1<=7) k |= b << 9;
        if (r+1<=7 && f-1>=0) k |= b << 7;
        if (r-1>=0 && f+1<=7) k |= b >> 7;
        if (r-1>=0 && f-1>=0) k |= b >> 9;
        king_attack_table[sq] = k;

        pawn_attack_table[WHITE][sq] = 0;
        pawn_attack_table[BLACK][sq] = 0;
        if (f+1<=7 && r+1<=7) pawn_attack_table[WHITE][sq] |= 1ULL << (sq+9);
        if (f-1>=0 && r+1<=7) pawn_attack_table[WHITE][sq] |= 1ULL << (sq+7);
        if (f+1<=7 && r-1>=0) pawn_attack_table[BLACK][sq] |= 1ULL << (sq-7);
        if (f-1>=0 && r-1>=0) pawn_attack_table[BLACK][sq] |= 1ULL << (sq-9);

        Bitboard mw = 0, mb = 0;
        for (int rr = r+1; rr <= 7; rr++) {
            if (f > 0) mw |= 1ULL << (rr*8 + f-1);
            mw |= 1ULL << (rr*8 + f);
            if (f < 7) mw |= 1ULL << (rr*8 + f+1);
        }
        for (int rr = r-1; rr >= 0; rr--) {
            if (f > 0) mb |= 1ULL << (rr*8 + f-1);
            mb |= 1ULL << (rr*8 + f);
            if (f < 7) mb |= 1ULL << (rr*8 + f+1);
        }
        passed_pawn_mask[WHITE][sq] = mw;
        passed_pawn_mask[BLACK][sq] = mb;

        Bitboard iso = 0;
        if (f > 0) for (int rr = 0; rr <= 7; rr++) iso |= 1ULL << (rr*8 + f-1);
        if (f < 7) for (int rr = 0; rr <= 7; rr++) iso |= 1ULL << (rr*8 + f+1);
        isolated_mask[sq] = iso;

        Bitboard fw = 0, fb = 0;
        for (int rr = r+1; rr <= 7; rr++) fw |= 1ULL << (rr*8+f);
        for (int rr = r-1; rr >= 0; rr--) fb |= 1ULL << (rr*8+f);
        forward_file_mask[WHITE][sq] = fw;
        forward_file_mask[BLACK][sq] = fb;
    }

    for (int f = 0; f < 8; f++) {
        file_bitboard[f] = FILE_A << f;
        adjacent_files[f] = 0;
        if (f > 0) adjacent_files[f] |= FILE_A << (f-1);
        if (f < 7) adjacent_files[f] |= FILE_A << (f+1);
    }

#ifdef USE_PEXT
    /* PEXT sliding tables must be ready before the between/ray loop below,
     * which calls rook_attacks()/bishop_attacks(). */
    {
        Bitboard subsets[4096];
        for (int sq = 0; sq < 64; sq++) {
            rook_masks[sq]   = compute_rook_mask(sq);
            bishop_masks[sq] = compute_bishop_mask(sq);

            int count = 0;
            Bitboard sub = 0;
            do {
                subsets[count++] = sub;
                sub = (sub - rook_masks[sq]) & rook_masks[sq];
            } while (sub);
            for (int i = 0; i < count; i++)
                rook_attack_table[sq][_pext_u64(subsets[i], rook_masks[sq])] = sliding_rook(sq, subsets[i]);

            count = 0; sub = 0;
            do {
                subsets[count++] = sub;
                sub = (sub - bishop_masks[sq]) & bishop_masks[sq];
            } while (sub);
            for (int i = 0; i < count; i++)
                bishop_attack_table[sq][_pext_u64(subsets[i], bishop_masks[sq])] = sliding_bishop(sq, subsets[i]);
        }
    }
#endif

    /* Between / ray tables */
    for (int a = 0; a < 64; a++) {
        for (int b2 = 0; b2 < 64; b2++) {
            between_table[a][b2] = 0;
            ray_table[a][b2] = 0;
            if (a == b2) continue;
            Bitboard ra = rook_attacks(a, 0), rb = rook_attacks(b2, 0);
            if (ra & (1ULL << b2)) {
                between_table[a][b2] = rook_attacks(a, 1ULL<<b2) & rook_attacks(b2, 1ULL<<a);
                ray_table[a][b2] = (ra & rb) | (1ULL<<a) | (1ULL<<b2);
                continue;
            }
            Bitboard ba = bishop_attacks(a, 0), bb = bishop_attacks(b2, 0);
            if (ba & (1ULL << b2)) {
                between_table[a][b2] = bishop_attacks(a, 1ULL<<b2) & bishop_attacks(b2, 1ULL<<a);
                ray_table[a][b2] = (ba & bb) | (1ULL<<a) | (1ULL<<b2);
            }
        }
    }

}
