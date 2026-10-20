/* inference.c — network inference.
 *
 * Network layout (little endian, no header):
 *   threat + pawn-pair weights   int8  [64368][1024]
 *   piece-square weights         int16 [16 buckets * 704][1024]
 *   feature biases               int16 [1024]
 *   L1 int8 [1024][8][32], L1 bias f32 [8][32]
 *   L2 f32 [32][8][64],    L2 bias f32 [8][64]
 *   L3 f32 [32][8],        L3 bias f32 [8]
 *
 * Each perspective keeps two accumulators: a king-bucketed piece-square
 * part (which also carries the biases) and an un-bucketed threat part
 * (attacker->victim edges and same/adjacent-file pawn pairs). The threat
 * part only depends on the king's half of the board, so a king move into a
 * new bucket refreshes the piece-square part alone, from a per-bucket cache
 * of the last accumulator seen in that bucket.
 *
 * Accumulators are evaluated lazily: a frame per ply is built on demand by
 * diffing against the nearest computed ancestor. Feature-transformer
 * neurons are stored in a sparsity-friendly order (see neuron_order[]) so
 * that whole 4-byte input groups of the L1 layer are zero more often. */

#define _GNU_SOURCE
#include "inference.h"
#include "bitboard.h"
#include <immintrin.h>
#include <assert.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#ifdef __linux__
#include <sys/mman.h>
#endif

#define WIDTH 1024
#define HALF  512
#define AUX   64368
#define PSQT  11264
#define CAP   1024
#define FRAMES (MAX_PLY + 16)
#define AUX_BYTES  ((size_t)AUX * WIDTH)
#define PSQT_BYTES ((size_t)PSQT * WIDTH * 2)
#define BIAS_OFFSET (AUX_BYTES + PSQT_BYTES)
#define L1_OFFSET  (BIAS_OFFSET + WIDTH * 2)
#define L1B_OFFSET (L1_OFFSET + 1024 * 8 * 32)
#define L2_OFFSET  (L1B_OFFSET + 8 * 32 * 4)
#define L2B_OFFSET (L2_OFFSET + 32 * 8 * 64 * 4)
#define L3_OFFSET  (L2B_OFFSET + 8 * 64 * 4)
#define L3B_OFFSET (L3_OFFSET + 32 * 8 * 4)
_Static_assert(L3B_OFFSET + 8 * 4 == NET_BYTES, "network size");

static unsigned char *data;
static size_t data_size;
static const int8_t *aw;              /* threat/pawn-pair rows */
static const int16_t *pw, *bias;      /* piece-square rows, biases */
static _Alignas(64) int8_t w1[8][32768];
static _Alignas(64) float b1[8][32], w2[8][32][64], b2[8][64], w3[8][32], b3[8];
static _Alignas(64) const int16_t zero_acc[WIDTH];
const char *net_tier = "scalar";

#include "neuron_order.h"

typedef struct {
    _Alignas(64) int16_t psqt[2][WIDTH];
    _Alignas(64) int16_t thr[2][WIDTH];
    Bitboard attf[64];      /* attack set of the non-king piece on each square */
    Bitboard men, pawns;    /* non-king occupancy and pawn occupancy */
    uint16_t pp[2][128];    /* pawn-pair feature ids per perspective */
    uint8_t npp[2];
    uint8_t squares[64], view[2], bucket[2];
    bool ready;
} Frame;

/* Last piece-square accumulator seen per perspective, bucket and mirror. */
typedef struct {
    _Alignas(64) int16_t acc[WIDTH];
    uint8_t squares[64];
} CacheEntry;

static _Thread_local Frame frames[FRAMES];
static _Thread_local CacheEntry bucket_cache[2][32];
static _Thread_local int base_ply;
static _Thread_local uint64_t seen_old[(AUX + 63) / 64], seen_new[(AUX + 63) / 64];

static uint16_t square_offset[12][64], piece_span[12];
static uint32_t piece_base[12];
static uint8_t square_rank[12][64][64];
static int32_t key_base[12][12];
static uint8_t key_dup[12][12];

static inline int16_t wrap(int v) { return (int16_t)(uint16_t)v; }
static inline int clip(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static Bitboard attacks(int piece, int sq, Bitboard occ) {
    switch (piece % 6) {
        case PAWN:   return pawn_attack_table[piece / 6][sq];
        case KNIGHT: return knight_attack_table[sq];
        case BISHOP: return bishop_attacks(sq, occ);
        case ROOK:   return rook_attacks(sq, occ);
        case QUEEN:  return queen_attacks(sq, occ);
        default:     return king_attack_table[sq];
    }
}

/* Victim slots per attacker type, as fixed by the network's feature schema. */
static int targets(int attacker, int victim) {
    if (attacker == PAWN) return victim == KNIGHT ? 0 : victim == ROOK ? 1 : -1;
    if (attacker == KING || victim == KING) return -1;
    if ((attacker == BISHOP || attacker == ROOK) && victim == QUEEN) return -1;
    return victim;
}
static int target_count(int t) { return t == PAWN ? 4 : t == KING ? 0 : (t == BISHOP || t == ROOK) ? 8 : 10; }

static void feature_tables(void) {
    uint32_t global = 0;
    for (int p = 0; p < 12; p++) {
        int n = 0;
        for (int s = 0; s < 64; s++) {
            square_offset[p][s] = (uint16_t)n;
            Bitboard bb = attacks(p, s, 0);
            for (int to = 0; to < 64; to++) square_rank[p][s][to] = (uint8_t)popcount(bb & ((1ULL << to) - 1));
            if (p % 6 != PAWN || (s >= 8 && s < 56)) n += popcount(bb);
        }
        piece_base[p] = global;
        piece_span[p] = (uint16_t)n;
        global += (uint32_t)(n * target_count(p % 6));
    }
    /* key = key_base[a][v] + square_offset[a][from] + square_rank[a][from][to]; -1 marks an absent
     * feature. A same-type pair is counted once: dropped when from < to in the perspective's frame. */
    for (int a = 0; a < 12; a++)
        for (int v = 0; v < 12; v++) {
            int local = targets(a % 6, v % 6);
            key_dup[a][v] = a % 6 == v % 6 && (a / 6 != v / 6 || a % 6 != PAWN);
            key_base[a][v] = local < 0 ? -1
                : (int32_t)(4560 + piece_base[a] + (uint32_t)((v / 6 * (target_count(a % 6) / 2) + local) * piece_span[a]));
        }
    assert(global == 59808);
}

static const unsigned char geometry[32] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 8, 9, 10, 11,
    12, 12, 13, 13, 12, 12, 13, 13, 14, 14, 15, 15, 14, 14, 15, 15};

static inline unsigned psqt_index(int bucket, int view, int piece, int sq, int p) {
    int t = piece % 6, c = t == KING ? 0 : ((piece / 6) ^ p);
    return (unsigned)(bucket * 704 + c * 384 + t * 64 + (sq ^ view));
}

/* Threat edges of the attackers in `attackers`, as feature ids for perspective p. */
static int edge_list(const Frame *fr, Bitboard attackers, int p, uint32_t *out, int n) {
    int fl = fr->view[p];
    for (Bitboard left = attackers & fr->men; left; left &= left - 1) {
        int from = __builtin_ctzll(left), ap = fr->squares[from];
        int a = (ap / 6 ^ p) * 6 + ap % 6, ff = from ^ fl;
        for (Bitboard t = fr->attf[from] & fr->men; t; t &= t - 1) {
            int to = __builtin_ctzll(t), vp = fr->squares[to];
            int v = (vp / 6 ^ p) * 6 + vp % 6, tt = to ^ fl;
            int32_t kb = key_base[a][v];
            if (kb >= 0 && !(key_dup[a][v] && ff < tt)) {
                assert(n < CAP);
                out[n++] = (uint32_t)kb + square_offset[a][ff] + square_rank[a][ff][tt];
            }
        }
    }
    return n;
}

static void pawn_pairs(const Board *b, Frame *f) {
    int sqs[16], colors[16], n = 0;
    Bitboard pawns = b->pieces[0][PAWN] | b->pieces[1][PAWN];
    f->npp[0] = f->npp[1] = 0;
    while (pawns) { int sq = lsb_pop(&pawns); sqs[n] = sq; colors[n++] = b->piece_on[sq] / 6; }
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            if (abs((sqs[i] & 7) - (sqs[j] & 7)) > 1) continue;
            for (int p = 0; p < 2; p++) {
                int flip = f->view[p];
                unsigned a = (unsigned)((sqs[i] ^ flip) - 8 + ((colors[i] ^ p) * 48));
                unsigned v = (unsigned)((sqs[j] ^ flip) - 8 + ((colors[j] ^ p) * 48));
                unsigned hi = a > v ? a : v, lo = a < v ? a : v;
                f->pp[p][f->npp[p]++] = (uint16_t)(hi * (hi - 1) / 2 + lo);
            }
        }
}

/* ---- accumulator row kernels: dst = src + sum(adds) - sum(subs) ---- */

typedef struct { const uint32_t *add, *sub; int nadd, nsub; } RowOps;

static void psqt_rows_scalar(int16_t *dst, const int16_t *src, const RowOps *o) {
    if (dst != src) memcpy(dst, src, WIDTH * sizeof(int16_t));
    for (int r = 0; r < o->nadd; r++) for (int j = 0; j < WIDTH; j++) dst[j] = wrap(dst[j] + pw[(size_t)o->add[r] * WIDTH + j]);
    for (int r = 0; r < o->nsub; r++) for (int j = 0; j < WIDTH; j++) dst[j] = wrap(dst[j] - pw[(size_t)o->sub[r] * WIDTH + j]);
}
static void aux_rows_scalar(int16_t *dst, const int16_t *src, const RowOps *o) {
    if (dst != src) memcpy(dst, src, WIDTH * sizeof(int16_t));
    for (int r = 0; r < o->nadd; r++) for (int j = 0; j < WIDTH; j++) dst[j] = wrap(dst[j] + aw[(size_t)o->add[r] * WIDTH + j]);
    for (int r = 0; r < o->nsub; r++) for (int j = 0; j < WIDTH; j++) dst[j] = wrap(dst[j] - aw[(size_t)o->sub[r] * WIDTH + j]);
}

/* Pull the next tile of every touched row toward the core while summing the current one. */
static inline void prefetch_rows(const char *base, size_t rowbytes, const RowOps *o, size_t off, size_t span) {
    for (int r = 0; r < o->nadd; r++) { const char *q = base + (size_t)o->add[r] * rowbytes + off; for (size_t k = 0; k < span; k += 64) __builtin_prefetch(q + k, 0, 3); }
    for (int r = 0; r < o->nsub; r++) { const char *q = base + (size_t)o->sub[r] * rowbytes + off; for (size_t k = 0; k < span; k += 64) __builtin_prefetch(q + k, 0, 3); }
}

__attribute__((target("avx2")))
static void psqt_rows_avx2(int16_t *dst, const int16_t *src, const RowOps *o) {
    prefetch_rows((const char *)pw, WIDTH * 2, o, 0, 256);
    for (int tile = 0; tile < WIDTH; tile += 128) {
        if (tile + 128 < WIDTH) prefetch_rows((const char *)pw, WIDTH * 2, o, (size_t)(tile + 128) * 2, 256);
        __m256i v[8];
        for (int k = 0; k < 8; k++) v[k] = _mm256_load_si256((const void *)(src + tile + k * 16));
        for (int r = 0; r < o->nadd; r++) {
            const int16_t *row = pw + (size_t)o->add[r] * WIDTH + tile;
            for (int k = 0; k < 8; k++) v[k] = _mm256_add_epi16(v[k], _mm256_load_si256((const void *)(row + k * 16)));
        }
        for (int r = 0; r < o->nsub; r++) {
            const int16_t *row = pw + (size_t)o->sub[r] * WIDTH + tile;
            for (int k = 0; k < 8; k++) v[k] = _mm256_sub_epi16(v[k], _mm256_load_si256((const void *)(row + k * 16)));
        }
        for (int k = 0; k < 8; k++) _mm256_store_si256((void *)(dst + tile + k * 16), v[k]);
    }
}

__attribute__((target("avx2")))
static void aux_rows_avx2(int16_t *dst, const int16_t *src, const RowOps *o) {
    prefetch_rows((const char *)aw, WIDTH, o, 0, 128);
    for (int tile = 0; tile < WIDTH; tile += 128) {
        if (tile + 128 < WIDTH) prefetch_rows((const char *)aw, WIDTH, o, (size_t)tile + 128, 128);
        __m256i v[8];
        for (int k = 0; k < 8; k++) v[k] = _mm256_load_si256((const void *)(src + tile + k * 16));
        for (int r = 0; r < o->nadd; r++) {
            const int8_t *row = aw + (size_t)o->add[r] * WIDTH + tile;
            for (int k = 0; k < 8; k++)
                v[k] = _mm256_add_epi16(v[k], _mm256_cvtepi8_epi16(_mm_load_si128((const void *)(row + k * 16))));
        }
        for (int r = 0; r < o->nsub; r++) {
            const int8_t *row = aw + (size_t)o->sub[r] * WIDTH + tile;
            for (int k = 0; k < 8; k++)
                v[k] = _mm256_sub_epi16(v[k], _mm256_cvtepi8_epi16(_mm_load_si128((const void *)(row + k * 16))));
        }
        for (int k = 0; k < 8; k++) _mm256_store_si256((void *)(dst + tile + k * 16), v[k]);
    }
}

__attribute__((target("avx512f,avx512bw")))
static void psqt_rows_avx512(int16_t *dst, const int16_t *src, const RowOps *o) {
    prefetch_rows((const char *)pw, WIDTH * 2, o, 0, 512);
    for (int tile = 0; tile < WIDTH; tile += 256) {
        if (tile + 256 < WIDTH) prefetch_rows((const char *)pw, WIDTH * 2, o, (size_t)(tile + 256) * 2, 512);
        __m512i v[8];
        for (int k = 0; k < 8; k++) v[k] = _mm512_load_si512((const void *)(src + tile + k * 32));
        for (int r = 0; r < o->nadd; r++) {
            const int16_t *row = pw + (size_t)o->add[r] * WIDTH + tile;
            for (int k = 0; k < 8; k++) v[k] = _mm512_add_epi16(v[k], _mm512_load_si512((const void *)(row + k * 32)));
        }
        for (int r = 0; r < o->nsub; r++) {
            const int16_t *row = pw + (size_t)o->sub[r] * WIDTH + tile;
            for (int k = 0; k < 8; k++) v[k] = _mm512_sub_epi16(v[k], _mm512_load_si512((const void *)(row + k * 32)));
        }
        for (int k = 0; k < 8; k++) _mm512_store_si512((void *)(dst + tile + k * 32), v[k]);
    }
}

__attribute__((target("avx512f,avx512bw")))
static void aux_rows_avx512(int16_t *dst, const int16_t *src, const RowOps *o) {
    prefetch_rows((const char *)aw, WIDTH, o, 0, 512);
    for (int tile = 0; tile < WIDTH; tile += 512) {
        if (tile + 512 < WIDTH) prefetch_rows((const char *)aw, WIDTH, o, (size_t)tile + 512, 512);
        __m512i v[16];
        for (int k = 0; k < 16; k++) v[k] = _mm512_load_si512((const void *)(src + tile + k * 32));
        for (int r = 0; r < o->nadd; r++) {
            const int8_t *row = aw + (size_t)o->add[r] * WIDTH + tile;
            for (int k = 0; k < 16; k++)
                v[k] = _mm512_add_epi16(v[k], _mm512_cvtepi8_epi16(_mm256_load_si256((const void *)(row + k * 32))));
        }
        for (int r = 0; r < o->nsub; r++) {
            const int8_t *row = aw + (size_t)o->sub[r] * WIDTH + tile;
            for (int k = 0; k < 16; k++)
                v[k] = _mm512_sub_epi16(v[k], _mm512_cvtepi8_epi16(_mm256_load_si256((const void *)(row + k * 32))));
        }
        for (int k = 0; k < 16; k++) _mm512_store_si512((void *)(dst + tile + k * 32), v[k]);
    }
}

static void (*psqt_rows)(int16_t *, const int16_t *, const RowOps *) = psqt_rows_scalar;
static void (*aux_rows)(int16_t *, const int16_t *, const RowOps *) = aux_rows_scalar;

static uint64_t square_diff(const uint8_t *x, const uint8_t *y) {
    uint64_t m = 0;
    for (int i = 0; i < 4; i++) {
        __m128i e = _mm_cmpeq_epi8(_mm_loadu_si128((const void *)(x + i * 16)), _mm_loadu_si128((const void *)(y + i * 16)));
        m |= (uint64_t)(~(unsigned)_mm_movemask_epi8(e) & 0xffffu) << (i * 16);
    }
    return m;
}

/* Piece-square accumulator from scratch (verification and fresh frames). */
static void psqt_scratch(Frame *f, int p) {
    uint32_t add[32];
    int n = 0;
    for (int sq = 0; sq < 64; sq++)
        if (f->squares[sq] != EMPTY_SQUARE) add[n++] = psqt_index(f->bucket[p], f->view[p], f->squares[sq], sq, p);
    RowOps ops = {add, NULL, n, 0};
    psqt_rows(f->psqt[p], bias, &ops);
}

/* Piece-square accumulator through the bucket cache: only the squares that
 * differ from the cached position of this bucket are updated. */
static void psqt_cached(Frame *f, int p) {
    CacheEntry *e = &bucket_cache[p][f->bucket[p] * 2 + ((f->view[p] & 7) != 0)];
    uint32_t add[64], sub[64];
    int na = 0, ns = 0;
    for (uint64_t diff = square_diff(e->squares, f->squares); diff; diff &= diff - 1) {
        int sq = __builtin_ctzll(diff), before = e->squares[sq], after = f->squares[sq];
        if (before != EMPTY_SQUARE) sub[ns++] = psqt_index(f->bucket[p], f->view[p], before, sq, p);
        if (after != EMPTY_SQUARE) add[na++] = psqt_index(f->bucket[p], f->view[p], after, sq, p);
    }
    RowOps ops = {add, sub, na, ns};
    psqt_rows(e->acc, e->acc, &ops);
    memcpy(e->squares, f->squares, 64);
    memcpy(f->psqt[p], e->acc, sizeof e->acc);
}

static void threats_scratch(Frame *f, int p) {
    uint32_t add[CAP];
    int n = edge_list(f, f->men, p, add, 0);
    for (int i = 0; i < f->npp[p]; i++) { assert(n < CAP); add[n++] = f->pp[p][i]; }
    RowOps ops = {add, NULL, n, 0};
    aux_rows(f->thr[p], zero_acc, &ops);
}

static void build_frame(Frame *f, const Board *b, const Frame *old, bool use_cache) {
    memcpy(f->squares, b->piece_on, 64);
    f->men = b->occ_all & ~(b->pieces[0][KING] | b->pieces[1][KING]);
    f->pawns = b->pieces[0][PAWN] | b->pieces[1][PAWN];
    for (int p = 0; p < 2; p++) {
        int king = lsb(b->pieces[p][KING]) ^ (p ? 56 : 0), mirror = (king & 7) >= 4 ? 7 : 0;
        f->view[p] = (uint8_t)((p ? 56 : 0) ^ mirror);
        f->bucket[p] = geometry[(king >> 3) * 4 + ((king ^ mirror) & 7)];
    }

    uint64_t changed = 0;
    Bitboard affected = 0;
    bool pawns_changed = true;
    if (old) {
        changed = square_diff(old->squares, b->piece_on);
        /* Affected attackers: pieces on changed squares, plus pieces whose old attack set touched a
         * changed square (a ray can only open or close through a square it attacked before). */
        memcpy(f->attf, old->attf, sizeof f->attf);
        affected = changed;
        for (Bitboard l = old->men & ~changed; l; l &= l - 1) {
            int s = __builtin_ctzll(l);
            if (old->attf[s] & changed) affected |= 1ULL << s;
        }
        for (Bitboard l = affected; l; l &= l - 1) {
            int s = __builtin_ctzll(l), ap = b->piece_on[s];
            f->attf[s] = (ap == EMPTY_SQUARE || ap % 6 == KING) ? 0 : attacks(ap, s, b->occ_all);
        }
        pawns_changed = ((old->pawns | f->pawns) & changed) != 0;
    } else {
        memset(f->attf, 0, sizeof f->attf);
        for (Bitboard l = f->men; l; l &= l - 1) {
            int s = __builtin_ctzll(l);
            f->attf[s] = attacks(b->piece_on[s], s, b->occ_all);
        }
    }
    if (old && !pawns_changed && old->view[0] == f->view[0] && old->view[1] == f->view[1]) {
        memcpy(f->pp, old->pp, sizeof f->pp);
        memcpy(f->npp, old->npp, sizeof f->npp);
    } else {
        pawn_pairs(b, f);
    }

    for (int p = 0; p < 2; p++) {
        /* Piece-square part. */
        if (old && old->view[p] == f->view[p] && old->bucket[p] == f->bucket[p]) {
            uint32_t add[64], sub[64];
            int na = 0, ns = 0;
            for (uint64_t left = changed; left; left &= left - 1) {
                int sq = __builtin_ctzll(left), before = old->squares[sq], after = b->piece_on[sq];
                if (before != EMPTY_SQUARE) sub[ns++] = psqt_index(f->bucket[p], f->view[p], before, sq, p);
                if (after != EMPTY_SQUARE) add[na++] = psqt_index(f->bucket[p], f->view[p], after, sq, p);
            }
            RowOps ops = {add, sub, na, ns};
            psqt_rows(f->psqt[p], old->psqt[p], &ops);
        } else if (use_cache) {
            psqt_cached(f, p);
        } else {
            psqt_scratch(f, p);
        }

        /* Threat part: only edges of affected attackers (and the pawn pairs, if a pawn moved) differ. */
        if (old && old->view[p] == f->view[p]) {
            uint32_t olde[CAP], newe[CAP], add[CAP], sub[CAP];
            int no = edge_list(old, affected, p, olde, 0), nn = edge_list(f, affected, p, newe, 0);
            int na = 0, ns = 0;
            if (pawns_changed) {
                for (int i = 0; i < old->npp[p]; i++) olde[no++] = old->pp[p][i];
                for (int i = 0; i < f->npp[p]; i++) newe[nn++] = f->pp[p][i];
            }
            /* Set difference through two bitsets, cleared again afterwards. */
            for (int i = 0; i < no; i++) { uint32_t id = olde[i]; seen_old[id >> 6] |= 1ULL << (id & 63); }
            for (int i = 0; i < nn; i++) {
                uint32_t id = newe[i];
                seen_new[id >> 6] |= 1ULL << (id & 63);
                if (!(seen_old[id >> 6] >> (id & 63) & 1)) add[na++] = id;
            }
            for (int i = 0; i < no; i++) { uint32_t id = olde[i]; if (!(seen_new[id >> 6] >> (id & 63) & 1)) sub[ns++] = id; }
            for (int i = 0; i < no; i++) seen_old[olde[i] >> 6] = 0;
            for (int i = 0; i < nn; i++) seen_new[newe[i] >> 6] = 0;
            RowOps ops = {add, sub, na, ns};
            aux_rows(f->thr[p], old->thr[p], &ops);
        } else {
            threats_scratch(f, p);
        }
    }
    f->ready = true;
}

static const Frame *sync_frame(const Board *b, Frame *scratch, bool fresh) {
    int idx = b->ply - base_ply;
    if (fresh || idx < 0 || idx >= FRAMES) { build_frame(scratch, b, NULL, false); return scratch; }
    Frame *f = &frames[idx];
    if (f->ready) return f;
    const Frame *old = NULL;
    for (int i = idx - 1; i >= 0; i--) if (frames[i].ready) { old = &frames[i]; break; }
    build_frame(f, b, old, true);
    return f;
}

/* ---- dense layers ---- */

static inline float hard_swish(float x) { float g = fminf(fmaxf(x + 3.0f, 0.0f), 6.0f); return (x * g) * (1.0f / 6.0f); }

static int finish_scalar(const int32_t *s, int bucket) {
    float x[32], z[64], h[32], sum[16] = {0};
    for (int j = 0; j < 32; j++) x[j] = hard_swish(fmaf((float)s[j], 512.0f / (255.0f * 255.0f * 64.0f), b1[bucket][j]));
    memcpy(z, b2[bucket], sizeof z);
    for (int i = 0; i < 32; i++) for (int j = 0; j < 64; j++) z[j] = fmaf(x[i], w2[bucket][i][j], z[j]);
    for (int i = 0; i < 32; i++) h[i] = hard_swish(z[i]) * z[i + 32] + x[i];
    for (int i = 0; i < 32; i++) sum[i % 16] = fmaf(h[i], w3[bucket][i], sum[i % 16]);
    float t[8], u[4];
    for (int i = 0; i < 8; i++) t[i] = sum[i] + sum[i + 8];
    for (int i = 0; i < 4; i++) u[i] = t[i] + t[i + 4];
    float y = ((u[0] + u[2]) + (u[1] + u[3])) + b3[bucket];
    return (int)(y * 240.0f);
}

/* Eight-lane float head; the FMA order matches finish_scalar exactly. */
__attribute__((target("avx2,fma")))
static int finish_avx2(const int32_t *s, int bucket) {
    _Alignas(64) float x[32], h[32];
    __m256 z[8], zero = _mm256_setzero_ps(), three = _mm256_set1_ps(3.0f), six = _mm256_set1_ps(6.0f),
           inv = _mm256_set1_ps(1.0f / 6.0f), scale = _mm256_set1_ps(512.0f / (255.0f * 255.0f * 64.0f));
    for (int j = 0; j < 4; j++) {
        __m256 v = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_load_si256((const void *)(s + j * 8))), scale,
                                   _mm256_load_ps(b1[bucket] + j * 8));
        __m256 g = _mm256_min_ps(_mm256_max_ps(_mm256_add_ps(v, three), zero), six);
        _mm256_store_ps(x + j * 8, _mm256_mul_ps(_mm256_mul_ps(v, g), inv));
    }
    for (int j = 0; j < 8; j++) z[j] = _mm256_load_ps(b2[bucket] + j * 8);
    for (int i = 0; i < 32; i++) {
        __m256 v = _mm256_set1_ps(x[i]);
        for (int j = 0; j < 8; j++) z[j] = _mm256_fmadd_ps(v, _mm256_load_ps(w2[bucket][i] + j * 8), z[j]);
    }
    for (int j = 0; j < 4; j++) {
        __m256 g = _mm256_min_ps(_mm256_max_ps(_mm256_add_ps(z[j], three), zero), six);
        __m256 sw = _mm256_mul_ps(_mm256_mul_ps(z[j], g), inv);
        _mm256_store_ps(h + j * 8, _mm256_add_ps(_mm256_mul_ps(sw, z[j + 4]), _mm256_load_ps(x + j * 8)));
    }
    __m256 a = _mm256_mul_ps(_mm256_load_ps(h), _mm256_load_ps(w3[bucket]));
    __m256 c = _mm256_mul_ps(_mm256_load_ps(h + 8), _mm256_load_ps(w3[bucket] + 8));
    a = _mm256_fmadd_ps(_mm256_load_ps(h + 16), _mm256_load_ps(w3[bucket] + 16), a);
    c = _mm256_fmadd_ps(_mm256_load_ps(h + 24), _mm256_load_ps(w3[bucket] + 24), c);
    __m256 total = _mm256_add_ps(a, c);
    __m128 t = _mm_add_ps(_mm256_castps256_ps128(total), _mm256_extractf128_ps(total, 1));
    t = _mm_add_ps(t, _mm_movehl_ps(t, t));
    t = _mm_add_ss(t, _mm_shuffle_ps(t, t, 1));
    return (int)((_mm_cvtss_f32(t) + b3[bucket]) * 240.0f);
}

static inline int output_bucket(const Board *b) { return (popcount(b->occ_all) - 2) / 4; }

/* Pairwise CReLU product of the summed accumulators, stm half first. */
static void activate_scalar(const Frame *f, const Board *b, uint8_t *x) {
    for (int p = 0; p < 2; p++) {
        int side = b->side ^ p;
        const int16_t *ps = f->psqt[side], *th = f->thr[side];
        for (int i = 0; i < HALF; i++) {
            int l = wrap(ps[i] + th[i]), r = wrap(ps[i + HALF] + th[i + HALF]);
            x[p * HALF + i] = (uint8_t)((clip(l) * clip(r)) >> 9);
        }
    }
}

__attribute__((target("avx2")))
static void activate_avx2(const Frame *f, const Board *b, uint8_t *x) {
    const __m256i zero = _mm256_setzero_si256(), cap = _mm256_set1_epi16(255);
    for (int p = 0; p < 2; p++) {
        int side = b->side ^ p;
        const int16_t *ps = f->psqt[side], *th = f->thr[side];
        for (int i = 0; i < HALF; i += 32) {
            __m256i l0 = _mm256_add_epi16(_mm256_load_si256((const void *)(ps + i)), _mm256_load_si256((const void *)(th + i)));
            __m256i l1 = _mm256_add_epi16(_mm256_load_si256((const void *)(ps + i + 16)), _mm256_load_si256((const void *)(th + i + 16)));
            __m256i r0 = _mm256_add_epi16(_mm256_load_si256((const void *)(ps + i + HALF)), _mm256_load_si256((const void *)(th + i + HALF)));
            __m256i r1 = _mm256_add_epi16(_mm256_load_si256((const void *)(ps + i + HALF + 16)), _mm256_load_si256((const void *)(th + i + HALF + 16)));
            l0 = _mm256_slli_epi16(_mm256_min_epi16(_mm256_max_epi16(l0, zero), cap), 7);
            l1 = _mm256_slli_epi16(_mm256_min_epi16(_mm256_max_epi16(l1, zero), cap), 7);
            r0 = _mm256_min_epi16(r0, cap);
            r1 = _mm256_min_epi16(r1, cap);
            /* (l << 7) * r >> 16 == l * r >> 9; negative right-hand values pack to zero. */
            __m256i q0 = _mm256_mulhi_epi16(l0, r0), q1 = _mm256_mulhi_epi16(l1, r1);
            _mm256_store_si256((void *)(x + p * HALF + i), _mm256_permute4x64_epi64(_mm256_packus_epi16(q0, q1), 0xd8));
        }
    }
}

static int forward_scalar(const Frame *f, const Board *b) {
    uint8_t x[WIDTH];
    activate_scalar(f, b, x);
    int32_t s[32] = {0};
    int bucket = output_bucket(b);
    for (int i = 0; i < WIDTH; i++)
        for (int j = 0; j < 32; j++) s[j] += x[i] * w1[bucket][(i / 4) * 128 + j * 4 + i % 4];
    return finish_scalar(s, bucket);
}

/* Indices of the 4-byte input groups with any nonzero byte, ascending. */
__attribute__((target("avx2")))
static inline int nonzero_groups(const uint8_t *x, uint16_t *idx) {
    const __m256i zero = _mm256_setzero_si256();
    int n = 0;
    for (int c = 0; c < 32; c++) {
        unsigned z = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_load_si256((const void *)(x + c * 32)), zero)));
        for (unsigned nz = ~z & 0xffu; nz; nz &= nz - 1) idx[n++] = (uint16_t)(c * 8 + __builtin_ctz(nz));
    }
    return n;
}

__attribute__((target("avx2,fma")))
static int forward_avx2(const Frame *f, const Board *b) {
    _Alignas(32) uint8_t x[WIDTH];
    activate_avx2(f, b, x);
    int bucket = output_bucket(b);
    __m256i s[4];
    for (int j = 0; j < 4; j++) s[j] = _mm256_setzero_si256();
    const __m256i one = _mm256_set1_epi16(1);
    uint16_t idx[256];
    int ng = nonzero_groups(x, idx);
    int gi = 0;
    for (; gi + 1 < ng; gi += 2) {
        int g0 = idx[gi], g1 = idx[gi + 1];
        uint32_t b0, bb1;
        memcpy(&b0, x + g0 * 4, 4);
        memcpy(&bb1, x + g1 * 4, 4);
        __m256i in0 = _mm256_set1_epi32((int)b0), in1 = _mm256_set1_epi32((int)bb1);
        for (int j = 0; j < 4; j++) {
            __m256i p0 = _mm256_maddubs_epi16(in0, _mm256_load_si256((const void *)(w1[bucket] + g0 * 128 + j * 32)));
            __m256i p1 = _mm256_maddubs_epi16(in1, _mm256_load_si256((const void *)(w1[bucket] + g1 * 128 + j * 32)));
            s[j] = _mm256_add_epi32(s[j], _mm256_add_epi32(_mm256_madd_epi16(p0, one), _mm256_madd_epi16(p1, one)));
        }
    }
    for (; gi < ng; gi++) {
        int g = idx[gi];
        uint32_t bits;
        memcpy(&bits, x + g * 4, 4);
        __m256i in = _mm256_set1_epi32((int)bits);
        for (int j = 0; j < 4; j++) {
            __m256i w = _mm256_load_si256((const void *)(w1[bucket] + g * 128 + j * 32));
            s[j] = _mm256_add_epi32(s[j], _mm256_madd_epi16(_mm256_maddubs_epi16(in, w), one));
        }
    }
    _Alignas(64) int32_t sums[32];
    for (int j = 0; j < 4; j++) _mm256_store_si256((void *)(sums + j * 8), s[j]);
    return finish_avx2(sums, bucket);
}

__attribute__((target("avx512f,avx512bw,avx512vnni,avx2,fma")))
static int forward_vnni(const Frame *f, const Board *b) {
    _Alignas(32) uint8_t x[WIDTH];
    activate_avx2(f, b, x);
    int bucket = output_bucket(b);
    __m512i lo = _mm512_setzero_si512(), hi = _mm512_setzero_si512();
    uint16_t idx[256];
    int ng = nonzero_groups(x, idx);
    for (int gi = 0; gi < ng; gi++) {
        int g = idx[gi];
        uint32_t bits;
        memcpy(&bits, x + g * 4, 4);
        __m512i in = _mm512_set1_epi32((int)bits);
        lo = _mm512_dpbusd_epi32(lo, in, _mm512_load_si512((const void *)(w1[bucket] + g * 128)));
        hi = _mm512_dpbusd_epi32(hi, in, _mm512_load_si512((const void *)(w1[bucket] + g * 128 + 64)));
    }
    _Alignas(64) int32_t sums[32];
    _mm512_store_si512(sums, lo);
    _mm512_store_si512(sums + 16, hi);
    return finish_avx2(sums, bucket);
}

static int (*forward)(const Frame *, const Board *) = forward_scalar;

/* ---- loading ---- */

static bool valid_order(void) {
    bool seen[HALF] = {false};
    for (int i = 0; i < HALF; i++) {
        if (neuron_order[i] >= HALF || seen[neuron_order[i]]) return false;
        seen[neuron_order[i]] = true;
    }
    return true;
}

bool net_load(const unsigned char *bytes, size_t count) {
    if (count != NET_BYTES) return false;
    /* f32 parameters must be finite before installing anything. */
    for (size_t i = L1B_OFFSET; i < count; i += 4) {
        float v;
        memcpy(&v, bytes + i, 4);
        if (!isfinite(v)) return false;
    }
    const size_t page = (size_t)2 << 20;
    size_t size = (count + page - 1) & ~(page - 1);
    unsigned char *fresh = rv_aligned_alloc(page, size);
    if (!fresh) return false;
#ifdef __linux__
    madvise(fresh, size, MADV_HUGEPAGE);   /* the weights are read at random: avoid TLB misses */
#endif
    /* Feature columns are stored in neuron_order: column j holds neuron order[j]. */
    bool permute = valid_order();
    uint16_t col[WIDTH];
    for (int j = 0; j < HALF; j++) {
        col[j] = permute ? neuron_order[j] : (uint16_t)j;
        col[j + HALF] = (uint16_t)(col[j] + HALF);
    }
    for (size_t r = 0; r < AUX; r++) {
        const int8_t *src = (const int8_t *)bytes + r * WIDTH;
        int8_t *dst = (int8_t *)fresh + r * WIDTH;
        for (int j = 0; j < WIDTH; j++) dst[j] = src[col[j]];
    }
    for (size_t r = 0; r < PSQT + 1; r++) {   /* piece-square rows and the bias row */
        int16_t src[WIDTH], dst[WIDTH];
        memcpy(src, bytes + AUX_BYTES + r * WIDTH * 2, sizeof src);
        for (int j = 0; j < WIDTH; j++) dst[j] = src[col[j]];
        memcpy(fresh + AUX_BYTES + r * WIDTH * 2, dst, sizeof dst);
    }
    for (int bucket = 0; bucket < 8; bucket++) {
        for (int i = 0; i < WIDTH; i++)
            for (int j = 0; j < 32; j++)
                w1[bucket][i / 4 * 128 + j * 4 + i % 4] = (int8_t)bytes[L1_OFFSET + ((size_t)col[i] * 8 + bucket) * 32 + j];
        memcpy(b1[bucket], bytes + L1B_OFFSET + bucket * 32 * 4, 32 * 4);
        for (int i = 0; i < 32; i++) memcpy(w2[bucket][i], bytes + L2_OFFSET + (i * 8 + bucket) * 64 * 4, 64 * 4);
        memcpy(b2[bucket], bytes + L2B_OFFSET + bucket * 64 * 4, 64 * 4);
        for (int i = 0; i < 32; i++) memcpy(&w3[bucket][i], bytes + L3_OFFSET + (i * 8 + bucket) * 4, 4);
        memcpy(&b3[bucket], bytes + L3B_OFFSET + bucket * 4, 4);
    }
    rv_aligned_free(data);
    data = fresh;
    data_size = size;
    aw = (const int8_t *)data;
    pw = (const int16_t *)(data + AUX_BYTES);
    bias = (const int16_t *)(data + BIAS_OFFSET);
    feature_tables();
    psqt_rows = psqt_rows_scalar;
    aux_rows = aux_rows_scalar;
    forward = forward_scalar;
    net_tier = "scalar";
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
        psqt_rows = psqt_rows_avx2;
        aux_rows = aux_rows_avx2;
        forward = forward_avx2;
        net_tier = "avx2";
    }
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")) {
        psqt_rows = psqt_rows_avx512;
        aux_rows = aux_rows_avx512;
        net_tier = "avx512";
        if (__builtin_cpu_supports("avx512vnni")) { forward = forward_vnni; net_tier = "avx512-vnni"; }
    }
    for (int i = 0; i < FRAMES; i++) frames[i].ready = false;
    return true;
}

void net_prepare(const Board *b) {
    base_ply = b->ply;
    for (int i = 0; i < FRAMES; i++) frames[i].ready = false;
    for (int p = 0; p < 2; p++)
        for (int e = 0; e < 32; e++) {
            memcpy(bucket_cache[p][e].acc, bias, sizeof bucket_cache[p][e].acc);
            memset(bucket_cache[p][e].squares, EMPTY_SQUARE, 64);
        }
    build_frame(&frames[0], b, NULL, true);
}

void net_push(const Board *b) {
    int i = b->ply - base_ply + 1;
    if (i > 0 && i < FRAMES) frames[i].ready = false;
}

int net_eval(const Board *b, bool fresh) {
    Frame scratch;
    return forward(sync_frame(b, &scratch, fresh), b);
}
