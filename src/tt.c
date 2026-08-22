/* tt.c — bucketed transposition table (depth-preferred + always-replace,
 * as popularised by Stockfish-classic engines) with generational ageing. */

#include "bitboard.h"
#include "tt.h"

typedef struct {
    uint32_t key;    /* upper 32 bits of hash */
    int16_t  score;
    uint16_t move;
    uint8_t  depth;
    uint8_t  bound;
    int8_t   age;
    uint8_t  pad;
} TTEntry;

typedef struct {
    TTEntry depth_preferred;
    TTEntry always_replace;
} TTBucket;

static TTBucket *tt_table = NULL;
static uint64_t  tt_buckets = 0;
static int8_t    tt_age = 0;

void tt_alloc(int mb) {
    if (tt_table) free(tt_table);
    if (mb < 1) mb = 1;
    tt_buckets = (uint64_t)mb * 1024 * 1024 / sizeof(TTBucket);
    tt_table = (TTBucket*)calloc(tt_buckets, sizeof(TTBucket));
    if (!tt_table) { fprintf(stderr, "TT allocation failed\n"); exit(1); }
}

void tt_clear(void) {
    if (tt_table) memset(tt_table, 0, tt_buckets * sizeof(TTBucket));
    tt_age = 0;
}

void tt_age_step(void) { tt_age++; }

static inline uint32_t tt_key(uint64_t hash) { return (uint32_t)(hash >> 32); }

static inline int tt_adjust_store(int score, int ply) {
    if (score >  MATE_BOUND) return score + ply;
    if (score < -MATE_BOUND) return score - ply;
    return score;
}
static inline int tt_adjust_retrieve(int score, int ply) {
    if (score >  MATE_BOUND) return score - ply;
    if (score < -MATE_BOUND) return score + ply;
    return score;
}

void tt_store(uint64_t hash, int score, Move move, int depth, int bound, int ply) {
    if (!tt_table) return;
    uint64_t index = (uint64_t)(((__uint128_t)hash * (__uint128_t)tt_buckets) >> 64);
    TTBucket *bucket = &tt_table[index];
    uint32_t key = tt_key(hash);
    score = tt_adjust_store(score, ply);

    TTEntry ne;
    ne.key   = key;
    ne.score = (int16_t)score;
    ne.move  = (uint16_t)move;
    ne.depth = (uint8_t)(depth < 255 ? depth : 255);
    ne.bound = (uint8_t)bound;
    ne.age   = tt_age;
    ne.pad   = 0;

    TTEntry *dp = &bucket->depth_preferred;
    if (dp->key == key || depth >= dp->depth || dp->age != tt_age)
        *dp = ne;
    bucket->always_replace = ne;
}

bool tt_probe(uint64_t hash, int *score, Move *move, int *depth, int *bound, int ply) {
    if (!tt_table) return false;
    uint64_t index = (uint64_t)(((__uint128_t)hash * (__uint128_t)tt_buckets) >> 64);
    TTBucket *bucket = &tt_table[index];
    uint32_t key = tt_key(hash);

    if (bucket->depth_preferred.key == key) {
        *score = tt_adjust_retrieve(bucket->depth_preferred.score, ply);
        *move  = bucket->depth_preferred.move;
        *depth = bucket->depth_preferred.depth;
        *bound = bucket->depth_preferred.bound;
        return true;
    }
    if (bucket->always_replace.key == key) {
        *score = tt_adjust_retrieve(bucket->always_replace.score, ply);
        *move  = bucket->always_replace.move;
        *depth = bucket->always_replace.depth;
        *bound = bucket->always_replace.bound;
        return true;
    }
    return false;
}

Move tt_probe_move(uint64_t hash) {
    int s; Move m; int d, bnd;
    if (tt_probe(hash, &s, &m, &d, &bnd, 0)) return m;
    return NO_MOVE;
}

int tt_hashfull(void) {
    if (!tt_table || tt_buckets == 0) return 0;
    int sample = 1000;
    if ((uint64_t)sample > tt_buckets) sample = (int)tt_buckets;
    int count = 0;
    for (int i = 0; i < sample; i++)
        if (tt_table[i].depth_preferred.key != 0 &&
            tt_table[i].depth_preferred.age == tt_age) count++;
    return count;
}
