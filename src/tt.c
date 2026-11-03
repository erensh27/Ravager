/* tt.c — bucketed transposition table.
 *
 * A 32-byte bucket holds three 10-byte entries. The 16-bit verification key
 * comes from the low bits of the Zobrist hash, while the bucket index comes
 * from the high bits (multiply-high), so the two never overlap. Atomic fields
 * avoid data races, while a bucket sequence serializes writers and lets
 * readers reject snapshots taken during replacement. Every TT move is
 * validated before use. */

#define _GNU_SOURCE
#include "bitboard.h"
#include "tt.h"
#include <limits.h>
#include <stdatomic.h>
#ifdef __linux__
#include <sys/mman.h>
#endif

typedef struct {
    _Atomic uint16_t key;
    _Atomic uint16_t move;
    _Atomic int16_t  score;
    _Atomic int16_t  eval;
    _Atomic uint8_t  depth;      /* depth + 1, 0 = empty slot */
    _Atomic uint8_t  genbound;   /* generation << 3 | pv << 2 | bound */
} TTEntry;

#define BUCKET_SIZE 3
typedef struct {
    TTEntry entry[BUCKET_SIZE];
    _Atomic uint16_t sequence;
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
    for (uint64_t i = 0; i < count; i++) {
        for (int j = 0; j < BUCKET_SIZE; j++) {
            TTEntry *e = &fresh[i].entry[j];
            atomic_init(&e->key, 0);
            atomic_init(&e->move, 0);
            atomic_init(&e->score, 0);
            atomic_init(&e->eval, 0);
            atomic_init(&e->depth, 0);
            atomic_init(&e->genbound, 0);
        }
        atomic_init(&fresh[i].sequence, 0);
    }
#ifdef __linux__
    madvise(fresh, count * sizeof(TTBucket), MADV_HUGEPAGE);
#endif
    rv_aligned_free(table);
    table = fresh;
    bucket_count = count;
    generation = 0;
    return true;
}

void tt_clear(void) {
    /* A zero depth marks an empty slot; other fields are ignored until reuse. */
    for (uint64_t i = 0; i < bucket_count; i++) {
        TTBucket *bucket = &table[i];
        uint16_t seq = atomic_load_explicit(&bucket->sequence, memory_order_relaxed);
        while (seq & 1 || !atomic_compare_exchange_weak_explicit(
                   &bucket->sequence, &seq, (uint16_t)(seq + 1),
                   memory_order_acquire, memory_order_relaxed)) {
            if (seq & 1) seq = atomic_load_explicit(&bucket->sequence, memory_order_relaxed);
        }
        for (int j = 0; j < BUCKET_SIZE; j++)
            atomic_store_explicit(&bucket->entry[j].depth, 0, memory_order_relaxed);
        atomic_store_explicit(&bucket->sequence, (uint16_t)(seq + 2), memory_order_release);
    }
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
static inline int age_of(uint8_t genbound) {
    return ((GEN_CYCLE + generation - genbound) & 0xF8) / GEN_STEP;
}

bool tt_probe(uint64_t hash, int ply, TTHit *hit) {
    TTBucket *bucket = bucket_of(hash);
    uint16_t key = (uint16_t)hash;
    for (int attempt = 0; attempt < 2; attempt++) {
        uint16_t seq = atomic_load_explicit(&bucket->sequence, memory_order_acquire);
        if (seq & 1) continue;
        bool found = false;
        TTHit snapshot;
        for (int i = 0; i < BUCKET_SIZE; i++) {
            TTEntry *e = &bucket->entry[i];
            uint8_t depth = atomic_load_explicit(&e->depth, memory_order_relaxed);
            if (!depth || atomic_load_explicit(&e->key, memory_order_relaxed) != key) continue;
            uint16_t move = atomic_load_explicit(&e->move, memory_order_relaxed);
            int16_t score = atomic_load_explicit(&e->score, memory_order_relaxed);
            int16_t eval = atomic_load_explicit(&e->eval, memory_order_relaxed);
            uint8_t genbound = atomic_load_explicit(&e->genbound, memory_order_relaxed);
            snapshot.move  = move;
            snapshot.score = score_from_tt(score, ply);
            snapshot.eval  = eval;
            snapshot.depth = depth - 1;
            snapshot.bound = genbound & 3;
            snapshot.pv    = (genbound >> 2) & 1;
            found = true;
            break;
        }
        uint16_t check = atomic_load_explicit(&bucket->sequence, memory_order_acquire);
        if (seq == check && !(check & 1)) {
            if (found) *hit = snapshot;
            return found;
        }
    }
    return false;
}

void tt_store(uint64_t hash, int ply, Move move, int score, int eval,
              int bound, int depth, bool pv) {
    TTBucket *bucket = bucket_of(hash);
    uint16_t seq = atomic_load_explicit(&bucket->sequence, memory_order_relaxed);
    for (;;) {
        if (seq & 1) {
            seq = atomic_load_explicit(&bucket->sequence, memory_order_relaxed);
            continue;
        }
        uint16_t expected = seq;
        if (atomic_compare_exchange_weak_explicit(&bucket->sequence, &expected,
                (uint16_t)(seq + 1), memory_order_acquire, memory_order_relaxed))
            break;
        seq = expected;
    }
    uint16_t key = (uint16_t)hash;
    TTEntry *slot = &bucket->entry[0];
    int slot_quality = INT_MAX;
    for (int i = 0; i < BUCKET_SIZE; i++) {
        TTEntry *e = &bucket->entry[i];
        uint8_t depth = atomic_load_explicit(&e->depth, memory_order_relaxed);
        if (!depth) { slot = e; break; }
        uint16_t entry_key = atomic_load_explicit(&e->key, memory_order_relaxed);
        uint8_t genbound = atomic_load_explicit(&e->genbound, memory_order_relaxed);
        if (entry_key == key) { slot = e; break; }
        /* Replace the shallowest, oldest entry. */
        int quality = (int)depth - 4 * age_of(genbound);
        if (quality < slot_quality) {
            slot = e;
            slot_quality = quality;
        }
    }
    if (depth < 0) depth = 0;
    if (depth > 250) depth = 250;
    uint8_t slot_depth = atomic_load_explicit(&slot->depth, memory_order_relaxed);
    bool same = false;
    uint8_t slot_genbound = 0;
    uint16_t slot_move = NO_MOVE;
    if (slot_depth) {
        uint16_t slot_key = atomic_load_explicit(&slot->key, memory_order_relaxed);
        slot_genbound = atomic_load_explicit(&slot->genbound, memory_order_relaxed);
        same = slot_key == key;
        slot_move = atomic_load_explicit(&slot->move, memory_order_relaxed);
    }
    if (move != NO_MOVE || !same) slot_move = move;
    /* Keep a deeper same-position entry unless the new one is exact. */
    if (same && bound != BOUND_EXACT && depth + 4 + 2 * pv <= slot_depth - 1 && age_of(slot_genbound) == 0) {
        if (move != NO_MOVE) {
            atomic_store_explicit(&slot->move, slot_move, memory_order_relaxed);
            atomic_store_explicit(&bucket->sequence, (uint16_t)(seq + 2), memory_order_release);
        } else {
            atomic_store_explicit(&bucket->sequence, (uint16_t)(seq + 2), memory_order_release);
        }
        return;
    }
    /* Hide the slot while its payload is replaced, then publish its depth. */
    atomic_store_explicit(&slot->depth, 0, memory_order_release);
    atomic_store_explicit(&slot->key, key, memory_order_relaxed);
    atomic_store_explicit(&slot->move, slot_move, memory_order_relaxed);
    atomic_store_explicit(&slot->score, (int16_t)score_to_tt(score, ply), memory_order_relaxed);
    atomic_store_explicit(&slot->eval, (int16_t)eval, memory_order_relaxed);
    atomic_store_explicit(&slot->genbound, (uint8_t)(generation | (pv ? 4 : 0) | (bound & 3)),
                          memory_order_relaxed);
    atomic_store_explicit(&slot->depth, (uint8_t)(depth + 1), memory_order_release);
    atomic_store_explicit(&bucket->sequence, (uint16_t)(seq + 2), memory_order_release);
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
            uint8_t depth = atomic_load_explicit(&e->depth, memory_order_relaxed);
            if (depth && age_of(atomic_load_explicit(&e->genbound, memory_order_relaxed)) == 0) count++;
        }
    return count * 1000 / (samples * BUCKET_SIZE);
}
