/* tt.c — lockless bucketed transposition table.
 *
 * A 32-byte bucket holds three 10-byte entries. The 16-bit verification key
 * comes from the low bits of the Zobrist hash, while the bucket index comes
 * from the high bits (multiply-high), so the two never overlap. Entries are
 * read and written without locks: a torn entry can at worst produce a bad
 * move or score, and every TT move is validated before use. */

#define _GNU_SOURCE
#include "bitboard.h"
#include "tt.h"
#ifdef __linux__
#include <sys/mman.h>
#endif

typedef struct {
    uint16_t key;
    uint16_t move;
    int16_t  score;
    int16_t  eval;
    uint8_t  depth;      /* depth + 1, 0 = empty slot */
    uint8_t  genbound;   /* generation << 3 | pv << 2 | bound */
} TTEntry;

#define BUCKET_SIZE 3
typedef struct {
    TTEntry entry[BUCKET_SIZE];
    uint16_t pad;
} TTBucket;
_Static_assert(sizeof(TTEntry) == 10, "TT entry layout");
_Static_assert(sizeof(TTBucket) == 32, "TT bucket layout");

#define GEN_STEP  8
#define GEN_CYCLE 256

static TTBucket *table = NULL;
static uint64_t bucket_count = 0;
static uint8_t generation = 0;

static inline TTBucket *bucket_of(uint64_t hash) {
    return &table[(uint64_t)(((__uint128_t)hash * bucket_count) >> 64)];
}

bool tt_alloc(int mb) {
    if (mb < 1) mb = 1;
    uint64_t bytes = (uint64_t)mb << 20;
    uint64_t count = bytes / sizeof(TTBucket);
    size_t align = bytes >= ((size_t)2 << 20) ? ((size_t)2 << 20) : 64;
    TTBucket *fresh = rv_aligned_alloc(align, count * sizeof(TTBucket));
    if (!fresh) {
        if (table) return false;   /* keep the old table */
        fprintf(stderr, "info string TT allocation failed\n");
        exit(1);
    }
#ifdef __linux__
    madvise(fresh, count * sizeof(TTBucket), MADV_HUGEPAGE);
#endif
    rv_aligned_free(table);
    table = fresh;
    bucket_count = count;
    tt_clear();
    return true;
}

void tt_clear(void) {
    if (table) memset(table, 0, bucket_count * sizeof(TTBucket));
    generation = 0;
}

void tt_new_search(void) { generation = (uint8_t)(generation + GEN_STEP); }

static inline int score_to_tt(int score, int ply) {
    if (score == SCORE_NONE) return score;
    if (score >= MATE_BOUND) return score + ply;
    if (score <= -MATE_BOUND) return score - ply;
    return score;
}

static inline int score_from_tt(int score, int ply) {
    if (score == SCORE_NONE) return score;
    if (score >= MATE_BOUND) return score - ply;
    if (score <= -MATE_BOUND) return score + ply;
    return score;
}

/* Age in generations, robust to wraparound. */
static inline int age_of(const TTEntry *e) {
    return ((GEN_CYCLE + generation - e->genbound) & 0xF8) / GEN_STEP;
}

bool tt_probe(uint64_t hash, int ply, TTHit *hit) {
    TTBucket *bucket = bucket_of(hash);
    uint16_t key = (uint16_t)hash;
    for (int i = 0; i < BUCKET_SIZE; i++) {
        TTEntry e = bucket->entry[i];
        if (e.key != key || !e.depth) continue;
        hit->move  = e.move;
        hit->score = score_from_tt(e.score, ply);
        hit->eval  = e.eval;
        hit->depth = e.depth - 1;
        hit->bound = e.genbound & 3;
        hit->pv    = (e.genbound >> 2) & 1;
        return true;
    }
    return false;
}

void tt_store(uint64_t hash, int ply, Move move, int score, int eval,
              int bound, int depth, bool pv) {
    TTBucket *bucket = bucket_of(hash);
    uint16_t key = (uint16_t)hash;
    TTEntry *slot = &bucket->entry[0];
    for (int i = 0; i < BUCKET_SIZE; i++) {
        TTEntry *e = &bucket->entry[i];
        if (!e->depth || e->key == key) { slot = e; break; }
        /* Replace the shallowest, oldest entry. */
        if ((int)e->depth - 4 * age_of(e) < (int)slot->depth - 4 * age_of(slot)) slot = e;
    }
    if (depth < 0) depth = 0;
    if (depth > 250) depth = 250;
    bool same = slot->depth && slot->key == key;
    if (move != NO_MOVE || !same) slot->move = move;
    /* Keep a deeper same-position entry unless the new one is exact. */
    if (same && bound != BOUND_EXACT && depth + 4 + 2 * pv <= slot->depth - 1 && age_of(slot) == 0)
        return;
    slot->key      = key;
    slot->score    = (int16_t)score_to_tt(score, ply);
    slot->eval     = (int16_t)eval;
    slot->depth    = (uint8_t)(depth + 1);
    slot->genbound = (uint8_t)(generation | (pv ? 4 : 0) | (bound & 3));
}

Move tt_probe_move(uint64_t hash) {
    TTHit hit;
    return tt_probe(hash, 0, &hit) ? hit.move : NO_MOVE;
}

void tt_prefetch(uint64_t hash) {
    if (table) __builtin_prefetch(bucket_of(hash));
}

int tt_hashfull(void) {
    if (!table) return 0;
    int count = 0, samples = bucket_count < 1000 ? (int)bucket_count : 1000;
    for (int i = 0; i < samples; i++)
        for (int j = 0; j < BUCKET_SIZE; j++) {
            const TTEntry *e = &table[i].entry[j];
            if (e->depth && age_of(e) == 0) count++;
        }
    return count * 1000 / (samples * BUCKET_SIZE);
}
