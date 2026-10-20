/* search.c — Ravager's search.
 *
 * Iterative deepening with aspiration windows over a principal variation
 * search. Each thread owns its board, network state and statistics; the
 * transposition table is shared (lazy SMP).
 *
 * Move ordering: staged picker (TT move, SEE-filtered captures, killer,
 * quiets, losing captures) scored by threat-aware butterfly/piece-to
 * history, 1/2/4-ply continuation history, pawn-structure history and
 * capture history.
 *
 * Static evaluation: network output scaled by remaining material, damped by
 * the fifty-move counter and corrected by pawn, non-pawn, minor-piece and
 * continuation correction histories.
 *
 * Selectivity: razoring, reverse futility pruning, null-move pruning with
 * verification, ProbCut with adaptive depth, internal iterative reduction,
 * hindsight depth adjustment, late move pruning, futility pruning, history
 * pruning, SEE pruning, singular/double/triple and negative extensions,
 * multi-cut, and history/node-type aware late move reductions with
 * deeper/shallower re-searches. Quiescence uses stand-pat, SEE ordering and
 * futility pruning, and searches all evasions when in check. */

#define _POSIX_C_SOURCE 200809L

#include "bitboard.h"
#include "tt.h"
#include "search.h"
#include "nnue.h"
#include "tb_syzygy.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

bool is_insufficient_material(const Board *b);  /* evaluate.c */
void eval_clear_pawn_hash(void);                /* evaluate.c */

int search_verbose = 1;
int move_overhead_ms = 20;

/* ------------------------------------------------------------------ */
/* Parameters (search-scale units: the network's centipawn-like scale). */

enum {
    ASP_DELTA = 12, ASP_DIVISOR = 30155, ASP_GROWTH = 43,
    RFP_MARGIN = 65, RFP_IMPROVING = 76,
    NMP_IMPROVING = 132, NMP_DEPTH_MUL = -8, NMP_EVAL_DIV = 174,
    SEE_QUIET_MARGIN = -62, SEE_NOISY_MARGIN = -28, SEE_HIST_MUL = 25,
    FUT_BASE = 86, FUT_MUL = 70,
    RAZOR_BASE = 123, RAZOR_MUL = 295,
    DEXT_MARGIN = 13, TEXT_MARGIN = 201,
    PROBCUT_MARGIN = 176, PROBCUT_IMPROVING = 78, PROBCUT_EVAL_DIV = 289,
    PROBCUT_SEE_SCALE = 266, PROBCUT_ADA_OFFSET = 50, PROBCUT_ADA_DIV = 300,
    HIST_LMR_DIV = 17017, QS_SEE_BOUND = -141, MAIN_SEE_BOUND = -93,
    DEEPER_BASE = 32, DEEPER_MUL = 8, SHALLOWER_MARGIN = 16,
    HIST_PRUNE_MARGIN = -3186, QS_FUTILITY = 350,
    LMR_KILLER = 775, LMR_NON_PV = 987, LMR_TTPV = 1289, LMR_TTPV_FAIL_LOW = 1136,
    LMR_CUT = 1601, LMR_NON_IMPROVING = 613, LMR_TT_CAPTURE = 999, LMR_CHECK = 1361,
    LMR_CORR = 448, LMR_ALPHA_RAISE = 384, LMR_OFFSET = 226, LMR_TTPV_DEPTH = 2057,
    LMR_DEPTH_OFFSET = -400, LMR_CUTOFFS = 900,
    MAIN_STAT = 26, CONT1_STAT = 37, CONT2_STAT = 33, CONT4_STAT = 13, TACT_STAT = 43,
    CORR_PAWN_W = 1890, CORR_MINOR_W = 1292, CORR_NONPAWN_W = 1887, CORR_CONT_W = 1942,
    POLICY_SCALE = 227, POLICY_OFFSET = -16, POLICY_MAX = 94,
    HINDSIGHT_EXT = 1419, HINDSIGHT_RED = 2494, HINDSIGHT_RED_EVAL = 128,
    OPTIMISM_OFFSET = 196, OPTIMISM_MAT_BASE = 1869, MATERIAL_SCALE_BASE = 856,
    QUIET_CHECK_SEE = 175,
};

typedef struct { int bonus_mul, bonus_off, bonus_max, malus_mul, malus_off, malus_max; } HistConf;
static const HistConf H_MAIN  = {357, 226, 2241, 111, 561, 915};
static const HistConf H_CONT1 = {287, 150, 3729, 270, 267, 1178};
static const HistConf H_CONT2 = {177, 178, 1596, 280, 130, 943};
static const HistConf H_CONT4 = {177, 185, 1630, 201, -32, 945};
static const HistConf H_PAWN  = {169, 162, 2208, 251, 188, 1281};
static const HistConf H_TACT  = {104, 328, 1248, 29, 394, 1122};

#define MAX_HISTORY     16383
#define CORR_SIZE       16384
#define CORR_MAX        1024
#define PAWN_HIST_SIZE  1024
#define KEY_HISTORY     1024
#define WINNING_BONUS   10000000
#define MIN_WINNING     (WINNING_BONUS - MAX_HISTORY * 4)
#define STACK_OFFSET    6

/* ------------------------------------------------------------------ */
/* Per-thread state. */

typedef int16_t PieceTo[12][64];

typedef struct {
    Move     move, excluded, killer;
    int      piece;            /* moved piece (colour*6+type), -1 for null */
    int      to;
    PieceTo *cont;             /* continuation entry of the move made here */
    int      static_eval, eval;
    int      reduction;        /* 1024ths of a ply */
    int      dext;             /* double extensions on the path */
    int      ply;
    int      plies_from_null;
    int      cutoffs;          /* fail-highs among this ply's siblings so far */
    bool     ttpv, tactical;
    Bitboard threats, threat_pawn, threat_minor, threat_rook;
} Stack;

typedef struct {
    Board    board;
    int      id;
    _Atomic uint64_t nodes;
    uint64_t tbhits;
    int      seldepth, root_depth, completed, score;
    Move     best_pv[MAX_PLY + 1];
    int      best_pv_len;
    Move     pv[MAX_PLY + 2][MAX_PLY + 2];
    int      pv_len[MAX_PLY + 2];
    Stack    stack[MAX_PLY + STACK_OFFSET + 4];
    uint64_t keys[KEY_HISTORY + MAX_PLY + 8];
    int      key_root;
    int      optimism[2];
    bool     nmp_banned[2];
    uint64_t root_nodes[64][64];
    int16_t  from_to[2][2][2][64][64];   /* [side][from threatened][to threatened] */
    int16_t  piece_to[2][2][12][64];
    int16_t  tactical[2][6][12][64];     /* [to threatened][captured][piece][to] */
    PieceTo  cont[12][64];
    PieceTo  pawn_hist[PAWN_HIST_SIZE];
    int16_t  corr_pawn[CORR_SIZE][2];
    int16_t  corr_nonpawn[2][CORR_SIZE][2];
    int16_t  corr_minor[CORR_SIZE][2];
    int16_t  corr_cont[CORR_SIZE][2];
} ThreadData;

static ThreadData **threads = NULL;
static int thread_count = 0;
static int lmr_table[64][64];
static int lmp_table[2][12];
static uint64_t cont_keys[13][64];

/* ------------------------------------------------------------------ */
/* Limits and time management (owned by the main thread). */

static atomic_bool stop_flag = false;
static atomic_bool pondering = false;
static pthread_mutex_t wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  wake_cond = PTHREAD_COND_INITIALIZER;
static SearchLimits limits;
static struct timespec start_ts;
static bool  time_managed;            /* clock-based ("dynamic") limits */
static double opt_time, hard_time, max_time, base_opt, base_hard;
static int   tm_stability, tm_failed_low;
static double tm_forced;              /* time multiplier once the best move is shown forced */
static Move  tm_prev_best;

static int64_t elapsed_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)(now.tv_sec - start_ts.tv_sec) * 1000 +
           (now.tv_nsec - start_ts.tv_nsec) / 1000000;
}

static uint64_t total_nodes(void) {
    uint64_t n = 0;
    for (int i = 0; i < thread_count; i++)
        n += atomic_load_explicit(&threads[i]->nodes, memory_order_relaxed);
    return n;
}

static uint64_t total_tbhits(void) {
    uint64_t n = 0;
    for (int i = 0; i < thread_count; i++) n += threads[i]->tbhits;
    return n;
}

static void setup_time(const Board *b) {
    int us = b->side;
    time_managed = false;
    opt_time = hard_time = max_time = 1e18;
    if (limits.infinite) return;
    if (limits.movetime > 0) {
        hard_time = opt_time = max_time = limits.movetime - move_overhead_ms > 1
                                        ? limits.movetime - move_overhead_ms : 1;
        return;
    }
    if (limits.time[us] <= 0 && limits.inc[us] <= 0) return;   /* depth/nodes/mate only */
    time_managed = true;
    double clock = limits.time[us] > 0 ? limits.time[us] : 0, inc = limits.inc[us];
    max_time = clock * 0.6 - move_overhead_ms;
    if (max_time < 1) max_time = 1;
    double hard = clock * 0.46;
    double opt;
    if (limits.movestogo > 0) {
        int mtg = limits.movestogo < 2 ? 2 : limits.movestogo > 24 ? 24 : limits.movestogo;
        opt = (clock / mtg < max_time ? clock / mtg : max_time) * 0.73;
        if (limits.movestogo == 1) opt = max_time * 0.73;
    } else {
        double computed = clock / 24 + inc * 0.94 - move_overhead_ms;
        if (computed < 1) computed = 1;
        opt = (computed < max_time ? computed : max_time) * 0.73;
    }
    if (hard > opt * 5) hard = opt * 5;
    if (hard > max_time) hard = max_time;
    if (opt > hard) opt = hard;
    base_opt = opt_time = opt;
    base_hard = hard_time = hard;
    tm_stability = 0;
    tm_failed_low = 0;
    tm_forced = 1.0;
    tm_prev_best = NO_MOVE;
}

static void tm_rescale(double node_fraction) {
    static const double stability_scale[5] = {2.50, 1.20, 0.90, 0.80, 0.75};
    double mult = stability_scale[tm_stability < 4 ? tm_stability : 4];
    mult *= 1.0 + 0.34 * tm_failed_low;
    if (node_fraction >= 0) mult *= (1.62 - node_fraction) * 1.40;
    mult *= tm_forced;
    opt_time = base_opt * mult;
    hard_time = base_hard * mult;
    if (opt_time > max_time) opt_time = max_time;
    if (hard_time > max_time) hard_time = max_time;
}

/* Called by the main thread inside the search. */
static inline bool should_stop(ThreadData *t) {
    if (atomic_load_explicit(&stop_flag, memory_order_relaxed)) return true;
    if (t->id != 0) return false;
    uint64_t n = atomic_load_explicit(&t->nodes, memory_order_relaxed);
    if (n & 1023) return false;
    if (limits.nodes && total_nodes() >= limits.nodes) {
        atomic_store(&stop_flag, true);
        return true;
    }
    if (!atomic_load_explicit(&pondering, memory_order_relaxed) && elapsed_ms() >= hard_time) {
        atomic_store(&stop_flag, true);
        return true;
    }
    return false;
}

static inline bool stopped(void) {
    return atomic_load_explicit(&stop_flag, memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* Small helpers. */

static inline bool is_decisive(int s) { return s >= TB_WIN_BOUND || s <= -TB_WIN_BOUND; }
static inline int  clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int  mated_in(int ply) { return -MATE_SCORE + ply; }
static inline int  mate_in(int ply) { return MATE_SCORE - ply; }

static inline void add_node(ThreadData *t) {
    atomic_store_explicit(&t->nodes, atomic_load_explicit(&t->nodes, memory_order_relaxed) + 1,
                          memory_order_relaxed);
}

static inline int draw_score(ThreadData *t) {
    return (int)(atomic_load_explicit(&t->nodes, memory_order_relaxed) & 2) - 1;
}

static inline int captured_type(const Board *b, Move m) {
    if (move_is_ep(m) || move_is_promo(m)) return PAWN;
    return b->piece_on[move_to(m)] % 6;
}

static inline bool has_bit(Bitboard bb, int sq) { return (bb >> sq) & 1; }

/* Squares attacked by the opponent, split by attacker class. */
static void compute_threats(const Board *b, Stack *ss) {
    int them = b->side ^ 1;
    Bitboard occ = b->occ_all, p = b->pieces[them][PAWN], att;
    att = them == WHITE ? (((p & NOT_FILE_A) << 7) | ((p & NOT_FILE_H) << 9))
                        : (((p & NOT_FILE_A) >> 9) | ((p & NOT_FILE_H) >> 7));
    ss->threat_pawn = att;
    for (Bitboard n = b->pieces[them][KNIGHT]; n; n &= n - 1) att |= knight_attack_table[lsb(n)];
    for (Bitboard n = b->pieces[them][BISHOP]; n; n &= n - 1) att |= bishop_attacks(lsb(n), occ);
    ss->threat_minor = att;
    for (Bitboard n = b->pieces[them][ROOK]; n; n &= n - 1) att |= rook_attacks(lsb(n), occ);
    ss->threat_rook = att;
    for (Bitboard n = b->pieces[them][QUEEN]; n; n &= n - 1) att |= queen_attacks(lsb(n), occ);
    att |= king_attack_table[lsb(b->pieces[them][KING])];
    ss->threats = att;
}

static int material_scale(const Board *b) {
    int n = popcount(b->pieces[0][KNIGHT] | b->pieces[1][KNIGHT] | b->pieces[0][BISHOP] | b->pieces[1][BISHOP]);
    int r = popcount(b->pieces[0][ROOK] | b->pieces[1][ROOK]);
    int q = popcount(b->pieces[0][QUEEN] | b->pieces[1][QUEEN]);
    return (see_values[KNIGHT] * n + see_values[ROOK] * r + see_values[QUEEN] * q) / 32;
}

/* ------------------------------------------------------------------ */
/* Histories. */

static inline int hist_bonus(const HistConf *c, int depth) {
    int v = c->bonus_mul * depth + c->bonus_off;
    return v < c->bonus_max ? v : c->bonus_max;
}
static inline int hist_malus(const HistConf *c, int depth) {
    int v = c->malus_mul * depth + c->malus_off;
    return v < c->malus_max ? v : c->malus_max;
}
static inline int hist_delta(const HistConf *c, int depth, bool good) {
    return good ? hist_bonus(c, depth) : -hist_malus(c, depth);
}

static inline void gravity(int16_t *v, int modulator, int delta, int max) {
    int nv = *v + delta - modulator * abs(delta) / max;
    *v = (int16_t)clampi(nv, -max, max);
}

static inline int main_history(const ThreadData *t, const Stack *ss, int side, int from, int to, int piece) {
    int ft = has_bit(ss->threats, from), tt = has_bit(ss->threats, to);
    return (t->piece_to[ft][tt][piece][to] + t->from_to[side][ft][tt][from][to]) / 2;
}

static inline int pawn_index(const Board *b) { return (int)(b->pawn_key % PAWN_HIST_SIZE); }

static inline int cont_value(const Stack *ss, int back, int piece, int to) {
    const Stack *p = ss - back;
    return p->cont ? (*p->cont)[piece][to] : 0;
}

static int quiet_stat(const ThreadData *t, const Stack *ss, Move m, int piece) {
    int from = move_from(m), to = move_to(m);
    return main_history(t, ss, t->board.side, from, to, piece) * MAIN_STAT
         + cont_value(ss, 1, piece, to) * CONT1_STAT
         + cont_value(ss, 2, piece, to) * CONT2_STAT
         + cont_value(ss, 4, piece, to) * CONT4_STAT;
}

static int tactical_stat(const ThreadData *t, const Stack *ss, Move m, int piece) {
    int to = move_to(m);
    return t->tactical[has_bit(ss->threats, to)][captured_type(&t->board, m)][piece][to] * TACT_STAT;
}

static void update_main(ThreadData *t, Bitboard threats, int side, int from, int to, int piece, int delta) {
    int ft = has_bit(threats, from), tt = has_bit(threats, to);
    gravity(&t->piece_to[ft][tt][piece][to], t->piece_to[ft][tt][piece][to], delta, MAX_HISTORY);
    gravity(&t->from_to[side][ft][tt][from][to], t->from_to[side][ft][tt][from][to], delta, MAX_HISTORY);
}

static void update_cont(Stack *ss, int piece, int to, int depth, bool good) {
    static const int backs[3] = {1, 2, 4}, muls[3] = {CONT1_STAT, CONT2_STAT, CONT4_STAT};
    static const HistConf *confs[3] = {&H_CONT1, &H_CONT2, &H_CONT4};
    int sum = 0;
    for (int i = 0; i < 3; i++) sum += muls[i] * cont_value(ss, backs[i], piece, to);
    sum /= 32;
    for (int i = 0; i < 3; i++) {
        Stack *p = ss - backs[i];
        if (!p->cont) continue;
        gravity(&(*p->cont)[piece][to], sum, hist_delta(confs[i], depth, good), MAX_HISTORY);
    }
}

static void update_quiet_single(ThreadData *t, Stack *ss, Move m, int depth, bool good) {
    const Board *b = &t->board;
    int from = move_from(m), to = move_to(m), piece = b->piece_on[from];
    update_main(t, ss->threats, b->side, from, to, piece, hist_delta(&H_MAIN, depth, good));
    update_cont(ss, piece, to, depth, good);
    int16_t *ph = &t->pawn_hist[pawn_index(b)][piece][to];
    gravity(ph, *ph, hist_delta(&H_PAWN, depth, good), MAX_HISTORY);
}

static void update_tactical(ThreadData *t, Stack *ss, const Move *moves, int n, Move best, int depth) {
    const Board *b = &t->board;
    int good = hist_delta(&H_TACT, depth, true), bad = hist_delta(&H_TACT, depth, false);
    for (int i = 0; i < n; i++) {
        Move m = moves[i];
        int to = move_to(m);
        int16_t *v = &t->tactical[has_bit(ss->threats, to)][captured_type(b, m)][b->piece_on[move_from(m)]][to];
        gravity(v, *v, m == best ? good : bad, MAX_HISTORY);
    }
}

/* Bonus/malus for the move that led to this node, from its own perspective. */
static void update_inbound(ThreadData *t, Stack *ss, int delta) {
    Stack *prev = ss - 1;
    if (prev->move == NO_MOVE || prev->tactical || prev->piece < 0) return;
    update_main(t, prev->threats, t->board.side ^ 1, move_from(prev->move), prev->to, prev->piece, delta);
}

/* Correction history. */
static inline int corr_sign(uint64_t key) { return 1 - 2 * (int)(key >> 63); }
static inline int16_t *corr_slot(int16_t table[][2], uint64_t key, int side) {
    return &table[key % CORR_SIZE][side];
}
static inline uint64_t cont_corr_key(const Stack *ss, int back) {
    const Stack *a = ss - 1, *c = ss - back;
    return cont_keys[a->piece < 0 ? 12 : a->piece][a->to] ^ cont_keys[c->piece < 0 ? 12 : c->piece][c->to];
}

static int correction(const ThreadData *t, const Stack *ss) {
    const Board *b = &t->board;
    int us = b->side;
    int64_t adj = 0;
    adj += (int64_t)CORR_PAWN_W * corr_sign(b->pawn_key) * t->corr_pawn[b->pawn_key % CORR_SIZE][us];
    adj += (int64_t)CORR_MINOR_W * corr_sign(b->minor_key) * t->corr_minor[b->minor_key % CORR_SIZE][us];
    for (int c = 0; c < 2; c++)
        adj += (int64_t)CORR_NONPAWN_W * corr_sign(b->nonpawn_key[c]) *
               t->corr_nonpawn[c][b->nonpawn_key[c] % CORR_SIZE][us];
    if (ss->ply > 2) {
        uint64_t k = cont_corr_key(ss, 2);
        adj += (int64_t)CORR_CONT_W * corr_sign(k) * t->corr_cont[k % CORR_SIZE][us];
    }
    if (ss->ply > 4) {
        uint64_t k = cont_corr_key(ss, 4);
        adj += (int64_t)CORR_CONT_W * corr_sign(k) * t->corr_cont[k % CORR_SIZE][us];
    }
    return (int)(adj * 12 / 0x40000);
}

static void update_correction(ThreadData *t, const Stack *ss, int depth, int complexity, int diff) {
    const Board *b = &t->board;
    int us = b->side;
    int factor = (int)((1.0f + log2f((float)complexity + 1.0f) / 10.0f) * 8.0f);
    int bonus = clampi(diff * depth * factor / 64, -CORR_MAX / 4, CORR_MAX / 4);
    int16_t *e;
#define CORR_UPDATE(slot, key) do { e = (slot); int sb = bonus * corr_sign(key); gravity(e, *e, sb, CORR_MAX); } while (0)
    CORR_UPDATE(corr_slot(t->corr_pawn, b->pawn_key, us), b->pawn_key);
    CORR_UPDATE(corr_slot(t->corr_minor, b->minor_key, us), b->minor_key);
    for (int c = 0; c < 2; c++)
        CORR_UPDATE(corr_slot(t->corr_nonpawn[c], b->nonpawn_key[c], us), b->nonpawn_key[c]);
    if (ss->ply > 2) { uint64_t k = cont_corr_key(ss, 2); CORR_UPDATE(corr_slot(t->corr_cont, k, us), k); }
    if (ss->ply > 4) { uint64_t k = cont_corr_key(ss, 4); CORR_UPDATE(corr_slot(t->corr_cont, k, us), k); }
#undef CORR_UPDATE
}

/* ------------------------------------------------------------------ */
/* Evaluation. */

static int raw_eval(ThreadData *t) {
    Board *b = &t->board;
    int v = (nnue_loaded && nnue_enabled) ? nnue_raw_output(b, false) : evaluate(b);
    return clampi(v, -EVAL_LIMIT + 1024, EVAL_LIMIT - 1024);
}

static int adjust_eval(const ThreadData *t, int raw, int corr) {
    const Board *b = &t->board;
    int material = material_scale(b);
    int v = (raw * (MATERIAL_SCALE_BASE + material) +
             t->optimism[b->side] * (OPTIMISM_MAT_BASE + material) / 32) / 1024;
    int clock = b->half_move_clock < 100 ? b->half_move_clock : 100;
    v = v * (200 - clock) / 200;
    return clampi(v + corr, -EVAL_LIMIT, EVAL_LIMIT);
}

/* ------------------------------------------------------------------ */
/* Draw detection. */

static bool is_draw(ThreadData *t, int ply) {
    const Board *b = &t->board;
    if (b->half_move_clock >= 100 || is_insufficient_material(b)) return true;
    int idx = t->key_root + ply;
    int stop = idx - b->half_move_clock;
    if (stop < 0) stop = 0;
    for (int i = idx - 4; i >= stop; i -= 2)
        if (t->keys[i] == b->hash) return true;
    return false;
}

/* A reversible move reaches a position seen earlier in the search tree. */
static bool upcoming_repetition(ThreadData *t, const Stack *ss) {
    int span = t->board.half_move_clock;
    if (span > ss->plies_from_null) span = ss->plies_from_null;
    if (span > ss->ply - 1) span = ss->ply - 1;
    return board_upcoming_repetition(&t->board, t->keys, t->key_root + ss->ply, span);
}

/* ------------------------------------------------------------------ */
/* Move picker. */

typedef enum { ST_TT, ST_GEN_NOISY, ST_GOOD_NOISY, ST_KILLER, ST_GEN_QUIET, ST_REMAINING, ST_DONE } Stage;

typedef struct {
    MoveList list;
    int   idx;
    Stage stage;
    Move  tt_move, killer;
    bool  skip_quiets;
    int   see_threshold;
} Picker;

static void picker_init(Picker *p, Move tt_move, Move killer, int see_threshold) {
    p->list.count = 0;
    p->idx = 0;
    p->stage = ST_TT;
    p->tt_move = tt_move;
    p->killer = killer;
    p->skip_quiets = false;
    p->see_threshold = see_threshold;
}

static void score_noisy(const ThreadData *t, const Stack *ss, MoveList *ml, int start) {
    static const int mvv[6] = {0, 2400, 2400, 4800, 9600, 0};
    const Board *b = &t->board;
    for (int i = start; i < ml->count; i++) {
        Move m = ml->moves[i];
        if (move_is_promo(m) && move_promo(m) != PROMO_QUEEN) { ml->scores[i] = -WINNING_BONUS; continue; }
        int to = move_to(m), cap = captured_type(b, m);
        ml->scores[i] = WINNING_BONUS + mvv[cap] +
                        t->tactical[has_bit(ss->threats, to)][cap][b->piece_on[move_from(m)]][to];
    }
}

static void score_quiets(const ThreadData *t, const Stack *ss, MoveList *ml, int start) {
    const Board *b = &t->board;
    int us = b->side, them = us ^ 1;
    const PieceTo *c1 = (ss - 1)->cont, *c2 = (ss - 2)->cont;
    const PieceTo *ph = &t->pawn_hist[pawn_index(b)];
    Bitboard their_king = b->pieces[them][KING], their_queens = b->pieces[them][QUEEN];
    Bitboard their_rooks = b->pieces[them][ROOK];
    Bitboard their_minors = b->pieces[them][KNIGHT] | b->pieces[them][BISHOP];
    Bitboard their_pawns = b->pieces[them][PAWN], our_pawns = b->pieces[us][PAWN];
    for (int i = start; i < ml->count; i++) {
        Move m = ml->moves[i];
        int from = move_from(m), to = move_to(m), piece = b->piece_on[from], pt = piece % 6;
        int s = main_history(t, ss, us, from, to, piece);
        if (c1) s += (*c1)[piece][to];
        if (c2) s += (*c2)[piece][to];
        s += (*ph)[piece][to];
        if (move_gives_check(b, m) && see_ge(b, m, -QUIET_CHECK_SEE)) s += 10000;
        switch (pt) {
            case PAWN:
                if (pawn_attack_table[them][to] & our_pawns) {   /* the pawn is defended there */
                    Bitboard a = pawn_attack_table[us][to];
                    if (a & their_king) s += 10000;
                    else if (a & their_queens) s += 8000;
                    else if (a & their_rooks) s += 6000;
                    else if (a & their_minors) s += 4000;
                    else if (a & their_pawns) s += 1000;
                }
                break;
            case KNIGHT: case BISHOP:
                if (has_bit(ss->threat_pawn, from)) s += 4000;
                if (has_bit(ss->threat_pawn, to)) s -= 4000;
                break;
            case ROOK:
                if (has_bit(ss->threat_minor, from)) s += 8000;
                if (has_bit(ss->threat_minor, to)) s -= 8000;
                break;
            case QUEEN:
                if (has_bit(ss->threat_rook, from)) s += 12000;
                if (has_bit(ss->threat_rook, to)) s -= 12000;
                break;
            default: break;
        }
        ml->scores[i] = s;
    }
}

/* Best remaining move, demoting captures that fail the SEE threshold. */
static int picker_select(Picker *p, const Board *b) {
    MoveList *ml = &p->list;
    while (p->idx < ml->count) {
        int best = p->idx;
        for (int i = p->idx + 1; i < ml->count; i++)
            if (ml->scores[i] > ml->scores[best]) best = i;
        if (ml->scores[best] >= MIN_WINNING && !see_ge(b, ml->moves[best], p->see_threshold)) {
            ml->scores[best] -= WINNING_BONUS;
            continue;
        }
        Move tm = ml->moves[p->idx]; ml->moves[p->idx] = ml->moves[best]; ml->moves[best] = tm;
        int ts = ml->scores[p->idx]; ml->scores[p->idx] = ml->scores[best]; ml->scores[best] = ts;
        return p->idx;
    }
    return -1;
}

static Move picker_next(Picker *p, ThreadData *t, const Stack *ss) {
    Board *b = &t->board;
    for (;;) switch (p->stage) {
    case ST_TT:
        p->stage = ST_GEN_NOISY;
        if (p->tt_move != NO_MOVE && (!p->skip_quiets || move_is_noisy(b, p->tt_move)))
            return p->tt_move;
        break;
    case ST_GEN_NOISY:
        generate_captures(b, &p->list);
        score_noisy(t, ss, &p->list, 0);
        p->stage = ST_GOOD_NOISY;
        break;
    case ST_GOOD_NOISY: {
        int i = picker_select(p, b);
        if (i >= 0 && p->list.scores[i] >= MIN_WINNING) {
            p->idx++;
            if (p->list.moves[i] == p->tt_move) continue;
            return p->list.moves[i];
        }
        p->stage = p->skip_quiets ? ST_DONE : ST_KILLER;
        break;
    }
    case ST_KILLER:
        p->stage = ST_GEN_QUIET;
        if (!p->skip_quiets && p->killer != NO_MOVE && p->killer != p->tt_move &&
            !move_is_noisy(b, p->killer) && move_is_pseudo_legal(b, p->killer))
            return p->killer;
        break;
    case ST_GEN_QUIET:
        p->stage = ST_REMAINING;
        if (!p->skip_quiets) {
            MoveList q;
            generate_quiets(b, &q);
            int start = p->list.count;
            for (int i = 0; i < q.count; i++) p->list.moves[p->list.count++] = q.moves[i];
            score_quiets(t, ss, &p->list, start);
        }
        break;
    case ST_REMAINING: {
        int i = picker_select(p, b);
        if (i < 0) { p->stage = ST_DONE; return NO_MOVE; }
        p->idx++;
        if (p->skip_quiets && p->list.scores[i] < MIN_WINNING) { p->stage = ST_DONE; return NO_MOVE; }
        Move m = p->list.moves[i];
        if (m == p->tt_move || m == p->killer) continue;
        return m;
    }
    case ST_DONE:
    default:
        return NO_MOVE;
    }
}

/* ------------------------------------------------------------------ */
/* Make/unmake wrappers that maintain the search stack. */

static inline void do_move(ThreadData *t, Stack *ss, Move m, bool tactical) {
    Board *b = &t->board;
    int piece = b->piece_on[move_from(m)];
    ss->move = m;
    ss->piece = piece;
    ss->to = move_to(m);
    ss->tactical = tactical;
    ss->cont = &t->cont[piece][ss->to];
    nnue_push_move(b, m);
    make_move(b, m);
    t->keys[t->key_root + ss->ply + 1] = b->hash;
    (ss + 1)->plies_from_null = ss->plies_from_null + 1;
    add_node(t);
}

static inline void undo_move(ThreadData *t, Move m) { unmake_move(&t->board, m); }

static inline void do_null(ThreadData *t, Stack *ss) {
    Board *b = &t->board;
    ss->move = NO_MOVE;
    ss->piece = -1;
    ss->to = 0;
    ss->tactical = false;
    ss->cont = &t->cont[b->side * 6 + PAWN][0];
    nnue_push_null(b);
    make_null_move(b);
    t->keys[t->key_root + ss->ply + 1] = b->hash;
    (ss + 1)->plies_from_null = 0;
    add_node(t);
}

static inline void update_pv(ThreadData *t, int ply, Move m) {
    t->pv[ply][0] = m;
    int n = t->pv_len[ply + 1];
    memcpy(&t->pv[ply][1], t->pv[ply + 1], (size_t)n * sizeof(Move));
    t->pv_len[ply] = n + 1;
}

static inline Move validated(const Board *b, Move m) {
    return (m != NO_MOVE && move_is_pseudo_legal(b, m) && move_is_legal(b, m)) ? m : NO_MOVE;
}

/* ------------------------------------------------------------------ */
/* Quiescence search. */

static int qsearch(ThreadData *t, Stack *ss, int alpha, int beta, bool pv_node) {
    Board *b = &t->board;
    int ply = ss->ply;
    if (should_stop(t)) return 0;
    if (pv_node) t->pv_len[ply] = 0;
    if (ply > t->seldepth) t->seldepth = ply;
    if (is_draw(t, ply)) return draw_score(t);
    bool in_check = b->checkers != 0;
    if (ply >= MAX_PLY - 1) return in_check ? 0 : adjust_eval(t, raw_eval(t), correction(t, ss));
    if (alpha < 0 && upcoming_repetition(t, ss)) {
        alpha = 0;
        if (alpha >= beta) return alpha;
    }

    TTHit hit;
    bool tt_hit = tt_probe(b->hash, ply, &hit);
    Move tt_move = NO_MOVE;
    if (tt_hit) {
        tt_move = validated(b, hit.move);
        if (hit.move != NO_MOVE && tt_move == NO_MOVE) tt_hit = false;
    }
    if (tt_hit && !pv_node && hit.score != SCORE_NONE && b->half_move_clock < 90 &&
        (hit.bound == BOUND_EXACT || (hit.bound == BOUND_LOWER && hit.score >= beta) ||
         (hit.bound == BOUND_UPPER && hit.score <= alpha)))
        return hit.score;
    ss->ttpv = pv_node || (tt_hit && hit.pv);

    int raw = SCORE_NONE, stand_pat, corr = 0;
    if (in_check) {
        stand_pat = -INFINITY_SCORE;
        ss->static_eval = SCORE_NONE;
    } else {
        corr = correction(t, ss);
        if (tt_hit) {
            raw = hit.eval != SCORE_NONE ? hit.eval : raw_eval(t);
            stand_pat = ss->static_eval = adjust_eval(t, raw, corr);
            if (hit.score != SCORE_NONE && !is_decisive(hit.score) &&
                (hit.bound == BOUND_EXACT || (hit.bound == BOUND_UPPER && hit.score < stand_pat) ||
                 (hit.bound == BOUND_LOWER && hit.score > stand_pat)))
                stand_pat = hit.score;
        } else {
            raw = raw_eval(t);
            tt_store(b->hash, ply, NO_MOVE, SCORE_NONE, raw, BOUND_NONE, 0, ss->ttpv);
            stand_pat = ss->static_eval = adjust_eval(t, raw, corr);
        }
        if (stand_pat >= beta)
            return is_decisive(stand_pat) ? stand_pat : (stand_pat + beta) / 2;
        if (stand_pat > alpha) alpha = stand_pat;
    }
    compute_threats(b, ss);

    int original_alpha = alpha, best_score = stand_pat, moves_made = 0;
    Move best_move = NO_MOVE;
    int futility = stand_pat + QS_FUTILITY;
    Picker p;
    picker_init(&p, tt_move, NO_MOVE, QS_SEE_BOUND);
    p.skip_quiets = !in_check;
    Move m;
    while ((m = picker_next(&p, t, ss)) != NO_MOVE) {
        if (!move_is_legal(b, m)) continue;
        bool tactical = move_is_noisy(b, m);
        if (best_score > -TB_WIN_BOUND && tactical && !in_check && futility <= alpha &&
            !is_decisive(futility) && move_to(m) != (ss - 1)->to &&
            !see_ge(b, m, 1) && !move_gives_check(b, m)) {
            if (best_score < futility) best_score = futility;
            continue;
        }
        do_move(t, ss, m, tactical);
        (ss + 1)->ply = ply + 1;
        if (best_score > -TB_WIN_BOUND) p.skip_quiets = true;
        moves_made++;
        int score = -qsearch(t, ss + 1, -beta, -alpha, pv_node);
        undo_move(t, m);
        if (stopped()) return 0;
        if (score > best_score) {
            best_score = score;
            if (score > alpha) {
                best_move = m;
                alpha = score;
                if (pv_node) update_pv(t, ply, m);
            }
            if (alpha >= beta) break;
        }
    }
    if (moves_made == 0 && in_check) return mated_in(ply);
    if (!is_decisive(best_score) && best_score > beta) best_score = (best_score + beta) / 2;
    int bound = best_score >= beta ? BOUND_LOWER : best_score > original_alpha ? BOUND_EXACT : BOUND_UPPER;
    tt_store(b->hash, ply, best_move, best_score, raw, bound, 0, ss->ttpv);
    return best_score;
}

/* ------------------------------------------------------------------ */
/* Main search. */

static bool zugzwang_unlikely(const Board *b) {
    int us = b->side;
    return (b->occupancy[us] & ~(b->pieces[us][PAWN] | b->pieces[us][KING])) != 0;
}

/* True when one of our pieces is attacked by a cheaper enemy piece. */
static bool hanging_material(const Board *b, const Stack *ss) {
    int us = b->side;
    Bitboard q = b->pieces[us][QUEEN], r = b->pieces[us][ROOK];
    Bitboard minors = b->pieces[us][BISHOP] | b->pieces[us][KNIGHT];
    return (ss->threat_rook & q) || (ss->threat_minor & (q | r)) || (ss->threat_pawn & (q | r | minors));
}

static int estimated_gain(const Board *b, Move m) {
    int v = (b->occ_all & (1ULL << move_to(m))) ? see_values[b->piece_on[move_to(m)] % 6] : 0;
    if (move_is_promo(m)) {
        static const int promo_piece[] = {KNIGHT, BISHOP, ROOK, QUEEN};
        v += see_values[promo_piece[move_promo(m)]] - see_values[PAWN];
    } else if (move_is_ep(m)) v = see_values[PAWN];
    return v;
}

static int negamax(ThreadData *t, Stack *ss, int alpha, int beta, int depth, bool cut_node, bool pv_node) {
    if (depth <= 0) return qsearch(t, ss, alpha, beta, pv_node);
    Board *b = &t->board;
    int ply = ss->ply;
    bool root = ply == 0;
    if (should_stop(t)) return 0;
    if (pv_node) t->pv_len[ply] = 0;
    if (ply > t->seldepth) t->seldepth = ply;
    if (depth > MAX_PLY - 1) depth = MAX_PLY - 1;
    bool in_check = b->checkers != 0;

    if (!root) {
        if (is_draw(t, ply)) return draw_score(t);
        if (ply >= MAX_PLY - 1) return in_check ? 0 : adjust_eval(t, raw_eval(t), correction(t, ss));
        alpha = alpha > mated_in(ply) ? alpha : mated_in(ply);
        beta = beta < mate_in(ply + 1) ? beta : mate_in(ply + 1);
        if (alpha >= beta) return alpha;
        if (alpha < 0 && upcoming_repetition(t, ss)) {
            alpha = 0;
            if (alpha >= beta) return alpha;
        }
    }

    Move excluded = ss->excluded;
    TTHit hit;
    bool tt_hit = false;
    Move tt_move = NO_MOVE;
    if (excluded == NO_MOVE && tt_probe(b->hash, ply, &hit)) {
        tt_hit = true;
        tt_move = validated(b, hit.move);
        if (hit.move != NO_MOVE && tt_move == NO_MOVE) tt_hit = false;
    }
    if (!tt_hit) { hit.score = SCORE_NONE; hit.eval = SCORE_NONE; hit.depth = -1; hit.bound = BOUND_NONE; hit.pv = false; }

    if (tt_hit && !pv_node && hit.score != SCORE_NONE && b->half_move_clock < 90) {
        bool proven = (hit.score >= TB_WIN_BOUND && hit.bound == BOUND_LOWER && hit.score >= beta) ||
                      (hit.score <= -TB_WIN_BOUND && hit.bound == BOUND_UPPER && hit.score <= alpha);
        if ((proven || hit.depth >= depth + (hit.score >= beta)) &&
            (hit.bound == BOUND_EXACT || (hit.bound == BOUND_LOWER && hit.score >= beta) ||
             (hit.bound == BOUND_UPPER && hit.score <= alpha))) {
            if (tt_move != NO_MOVE && hit.score >= beta && !move_is_noisy(b, tt_move)) {
                compute_threats(b, ss);
                update_quiet_single(t, ss, tt_move, depth, true);
            }
            return hit.score;
        }
    }
    compute_threats(b, ss);
    if (excluded == NO_MOVE) ss->ttpv = pv_node || (tt_hit && hit.pv);

    /* Tablebases. */
    int syzygy_min = -MATE_SCORE, syzygy_max = MATE_SCORE, wdl;
    if (!root && excluded == NO_MOVE && syzygy_probe_wdl(b, &wdl)) {
        t->tbhits++;
        int tb_score = wdl > 0 ? TB_WIN_SCORE - ply : wdl < 0 ? -TB_WIN_SCORE + ply : draw_score(t);
        int tb_bound = wdl > 0 ? BOUND_LOWER : wdl < 0 ? BOUND_UPPER : BOUND_EXACT;
        if (tb_bound == BOUND_EXACT || (tb_bound == BOUND_LOWER && tb_score >= beta) ||
            (tb_bound == BOUND_UPPER && tb_score <= alpha)) {
            tt_store(b->hash, ply, NO_MOVE, tb_score, SCORE_NONE, tb_bound,
                     depth + 6 < MAX_PLY - 1 ? depth + 6 : MAX_PLY - 1, ss->ttpv);
            return tb_score;
        }
        if (pv_node && tb_bound == BOUND_LOWER) { if (tb_score > alpha) alpha = tb_score; syzygy_min = tb_score; }
        if (pv_node && tb_bound == BOUND_UPPER) syzygy_max = tb_score;
    }

    /* Static evaluation. */
    int raw = SCORE_NONE, static_eval, eval, corr = correction(t, ss);
    if (in_check) {
        static_eval = eval = SCORE_NONE;
    } else if (excluded != NO_MOVE) {
        static_eval = ss->static_eval;
        eval = ss->eval;
    } else if (tt_hit) {
        raw = hit.eval != SCORE_NONE ? hit.eval : raw_eval(t);
        static_eval = eval = adjust_eval(t, raw, corr);
        if (hit.score != SCORE_NONE &&
            (hit.bound == BOUND_EXACT || (hit.bound == BOUND_UPPER && hit.score < static_eval) ||
             (hit.bound == BOUND_LOWER && hit.score > static_eval)))
            eval = hit.score;
    } else {
        raw = raw_eval(t);
        tt_store(b->hash, ply, NO_MOVE, SCORE_NONE, raw, BOUND_NONE, 0, ss->ttpv);
        static_eval = eval = adjust_eval(t, raw, corr);
    }
    ss->static_eval = static_eval;
    ss->eval = eval;

    int complexity = 0;
    if (tt_hit && static_eval != SCORE_NONE && hit.score != SCORE_NONE && !is_decisive(hit.score) &&
        (hit.bound == BOUND_EXACT || (hit.bound == BOUND_UPPER && hit.score < static_eval) ||
         (hit.bound == BOUND_LOWER && hit.score > static_eval)))
        complexity = abs(static_eval - hit.score);

    /* The previous quiet move is rewarded when it improved the mover's eval. */
    if (!root && (ss - 1)->move != NO_MOVE && (ss - 1)->static_eval != SCORE_NONE &&
        static_eval != SCORE_NONE && !(ss - 1)->tactical && excluded == NO_MOVE) {
        int improvement = -((ss - 1)->static_eval + static_eval) + POLICY_OFFSET;
        update_inbound(t, ss, clampi(improvement * POLICY_SCALE / 32, -POLICY_MAX, POLICY_MAX));
    }

    bool improving = false;
    if (!in_check) {
        if (ply >= 2 && (ss - 2)->static_eval != SCORE_NONE) improving = static_eval > (ss - 2)->static_eval;
        else if (ply >= 4 && (ss - 4)->static_eval != SCORE_NONE) improving = static_eval > (ss - 4)->static_eval;
        else improving = true;
    }
    ss->dext = root ? 0 : (ss - 1)->dext;
    (ss + 1)->killer = NO_MOVE;
    (ss + 2)->cutoffs = 0;
    bool tt_capture = tt_move != NO_MOVE && move_is_noisy(b, tt_move);

    if (!root && !pv_node && !in_check && excluded == NO_MOVE) {
        /* Hindsight depth adjustment from the parent's reduction. */
        if ((ss - 1)->reduction >= HINDSIGHT_EXT && (ss - 1)->static_eval != SCORE_NONE &&
            static_eval + (ss - 1)->static_eval < 0)
            depth++;
        if (depth >= 2 && (ss - 1)->reduction >= HINDSIGHT_RED && (ss - 1)->static_eval != SCORE_NONE &&
            static_eval + (ss - 1)->static_eval > HINDSIGHT_RED_EVAL)
            depth--;

        /* Razoring. */
        if (alpha < 2000 && static_eval < alpha - RAZOR_BASE - RAZOR_MUL * depth) {
            int v = qsearch(t, ss, alpha, beta, false);
            if (v <= alpha) return v;
        }

        /* Reverse futility pruning. */
        int rfp = RFP_MARGIN * depth - (improving && !hanging_material(b, ss)) * RFP_IMPROVING + abs(corr) / 2;
        if (!ss->ttpv && depth < 9 && beta > -TB_WIN_BOUND && eval < TB_WIN_BOUND && eval >= beta &&
            (tt_move == NO_MOVE || tt_capture) && eval - rfp >= beta)
            return beta + (eval - beta) / 3;

        /* Null-move pruning. */
        if (cut_node && (ss - 1)->move != NO_MOVE && depth > 2 &&
            static_eval + improving * NMP_IMPROVING + depth * NMP_DEPTH_MUL >= beta &&
            !t->nmp_banned[b->side] && zugzwang_unlikely(b) &&
            !(tt_hit && hit.bound == BOUND_UPPER && hit.score != SCORE_NONE && hit.score < beta) &&
            !(tt_hit && hit.bound == BOUND_LOWER && tt_move != NO_MOVE &&
              estimated_gain(b, tt_move) > see_values[PAWN] * 2)) {
            int r = 4 + depth / 3 + clampi((static_eval - beta) / NMP_EVAL_DIV, -100, 4) + tt_capture;
            int nm_depth = depth - r;
            ss->reduction = 0;
            do_null(t, ss);
            (ss + 1)->ply = ply + 1;
            int null_score = -negamax(t, ss + 1, -beta, -beta + 1, nm_depth, false, false);
            unmake_null_move(b);
            if (stopped()) return 0;
            if (null_score >= beta) {
                if (depth < 12 && !is_decisive(beta))
                    return is_decisive(null_score) ? beta : null_score;
                t->nmp_banned[b->side] = true;
                int verify = negamax(t, ss, beta - 1, beta, nm_depth, false, false);
                t->nmp_banned[b->side] = false;
                if (verify >= beta) return verify;
            }
        }
    }

    /* Internal iterative reduction. */
    if (excluded == NO_MOVE) {
        if (cut_node && depth >= 8 && (tt_move == NO_MOVE || hit.depth + 4 <= depth)) depth--;
        else if (pv_node && depth >= 4 && tt_move == NO_MOVE) depth--;
    }

    /* ProbCut. */
    int pc_beta = beta + PROBCUT_MARGIN - improving * PROBCUT_IMPROVING;
    if (pc_beta > TB_WIN_BOUND - 1) pc_beta = TB_WIN_BOUND - 1;
    if (cut_node && !in_check && excluded == NO_MOVE && depth >= 3 && !is_decisive(beta) &&
        !(tt_hit && hit.score != SCORE_NONE && hit.score < pc_beta)) {
        int depth_base = depth - 3 - (static_eval - beta) / PROBCUT_EVAL_DIV;
        int see_pivot = (pc_beta - static_eval) * PROBCUT_SEE_SCALE / 256;
        Picker p;
        picker_init(&p, tt_capture ? tt_move : NO_MOVE, NO_MOVE, see_pivot);
        p.skip_quiets = true;
        Move m;
        while ((m = picker_next(&p, t, ss)) != NO_MOVE) {
            if (!move_is_legal(b, m)) continue;
            do_move(t, ss, m, true);
            (ss + 1)->ply = ply + 1;
            int value = -qsearch(t, ss + 1, -pc_beta, -pc_beta + 1, false);
            int pc_depth = clampi(depth_base - clampi((value - pc_beta - PROBCUT_ADA_OFFSET) / PROBCUT_ADA_DIV, 0, 3),
                                  0, depth - 1);
            int base_depth = clampi(depth_base, 0, depth - 1);
            int ada_beta = clampi(pc_beta + (base_depth - pc_depth) * PROBCUT_ADA_DIV,
                                  -TB_WIN_BOUND + 1, TB_WIN_BOUND - 1);
            if (value >= pc_beta && pc_depth > 0) {
                value = -negamax(t, ss + 1, -ada_beta, -ada_beta + 1, pc_depth, false, false);
                if (value < ada_beta && pc_beta < ada_beta) {
                    pc_depth = base_depth;
                    value = -negamax(t, ss + 1, -pc_beta, -pc_beta + 1, pc_depth, false, false);
                } else {
                    pc_beta = ada_beta;
                }
            }
            undo_move(t, m);
            if (stopped()) return 0;
            if (value >= pc_beta) {
                tt_store(b->hash, ply, m, value, raw, BOUND_LOWER, pc_depth + 1, ss->ttpv);
                return is_decisive(value) ? value : value - (pc_beta - beta);
            }
        }
    }

    /* Singular extension. */
    int sext = 0;
    if (!root && tt_move != NO_MOVE && excluded == NO_MOVE && depth >= 6 + ss->ttpv &&
        hit.score != SCORE_NONE && !is_decisive(hit.score) &&
        (hit.bound == BOUND_LOWER || hit.bound == BOUND_EXACT) && hit.depth >= depth - 3 &&
        ply < t->root_depth * 2) {
        int r_beta = hit.score - depth * 48 / 64;
        int r_depth = (depth - 1) / 2;
        bool quiet = !move_is_noisy(b, tt_move);
        ss->excluded = tt_move;
        int value = negamax(t, ss, r_beta - 1, r_beta, r_depth, cut_node, false);
        ss->excluded = NO_MOVE;
        if (stopped()) return 0;
        if (value == SCORE_NONE) sext = 1;
        else if (value < r_beta) {
            if (!pv_node && ss->dext <= 12 && value < r_beta - DEXT_MARGIN)
                sext = 2 + (quiet && value < r_beta - TEXT_MARGIN);
            else
                sext = 1;
        } else if (!pv_node && value >= beta) {
            return value;   /* multi-cut */
        } else if (hit.score >= beta) {
            sext = -3 + pv_node;
        } else if (cut_node) {
            sext = -2;
        }
    }

    int original_alpha = alpha, best_score = -INFINITY_SCORE, moves_made = 0, alpha_raises = 0;
    Move best_move = NO_MOVE;
    Move quiets[MAX_MOVES], tacticals[MAX_MOVES];
    int n_quiets = 0, n_tacticals = 0;
    int lmp_threshold = lmp_table[improving][depth < 11 ? depth : 11];
    Move killer = (ss->killer != NO_MOVE && !move_is_noisy(b, ss->killer)) ? ss->killer : NO_MOVE;
    int see_table[2] = {SEE_NOISY_MARGIN * depth * depth, SEE_QUIET_MARGIN * depth};

    Picker p;
    picker_init(&p, tt_move, killer, MAIN_SEE_BOUND);
    Move m;
    while ((m = picker_next(&p, t, ss)) != NO_MOVE) {
        if (m == excluded) continue;
        if (!move_is_legal(b, m)) continue;
        bool quiet = !move_is_noisy(b, m);
        int from = move_from(m), piece = b->piece_on[from];
        int stat = quiet ? quiet_stat(t, ss, m, piece) / 32 : tactical_stat(t, ss, m, piece) / 32;

        int lmr = lmr_table[depth < 63 ? depth : 63][moves_made + 1 < 63 ? moves_made + 1 : 63];
        lmr += LMR_OFFSET;
        lmr += !pv_node * LMR_NON_PV;
        lmr -= ss->ttpv * LMR_TTPV;
        lmr += (ss->ttpv && hit.score != SCORE_NONE && hit.score <= alpha) * LMR_TTPV_FAIL_LOW;
        lmr += cut_node * LMR_CUT;
        lmr -= stat * 1024 / HIST_LMR_DIV;
        lmr -= (m == killer) * LMR_KILLER;
        lmr += !improving * LMR_NON_IMPROVING;
        lmr += tt_capture * LMR_TT_CAPTURE;
        lmr -= abs(corr) * LMR_CORR / 16384;
        lmr += alpha_raises * LMR_ALPHA_RAISE;
        lmr += ((ss + 1)->cutoffs > 2) * LMR_CUTOFFS;
        int lmr_depth = depth - (lmr + LMR_TTPV_DEPTH * ss->ttpv + LMR_DEPTH_OFFSET) / 1024;
        if (lmr_depth < 0) lmr_depth = 0;

        if (!root && !pv_node && !in_check && best_score > -TB_WIN_BOUND) {
            if (lmr_depth < 9 && moves_made >= lmp_threshold) p.skip_quiets = true;
            if (quiet && m != killer && lmr_depth < 7 && stat < HIST_PRUNE_MARGIN * (depth - 1)) {
                p.skip_quiets = true;
                continue;
            }
            if (quiet && lmr_depth < 6 && static_eval + lmr_depth * FUT_MUL + FUT_BASE + stat / 128 <= alpha &&
                !move_gives_check(b, m))
                p.skip_quiets = true;
        }
        if (!root && best_score > -TB_WIN_BOUND && p.stage > ST_GOOD_NOISY &&
            !see_ge(b, m, see_table[quiet] - stat * SEE_HIST_MUL / 1024))
            continue;

        if (quiet) { if (n_quiets < MAX_MOVES) quiets[n_quiets++] = m; }
        else if (n_tacticals < MAX_MOVES) tacticals[n_tacticals++] = m;

        uint64_t nodes_before = atomic_load_explicit(&t->nodes, memory_order_relaxed);
        moves_made++;
        int ext = m == tt_move ? sext : 0;
        if (ext >= 2) ss->dext++;

        do_move(t, ss, m, !quiet);
        (ss + 1)->ply = ply + 1;
        int score;
        if (moves_made == 1) {
            ss->reduction = 0;
            score = -negamax(t, ss + 1, -beta, -alpha, depth + ext - 1, !pv_node && !cut_node, pv_node);
        } else {
            int r;
            if (depth > 2 && moves_made > 1 + root) {
                r = lmr - (b->checkers != 0) * LMR_CHECK;
                ss->reduction = r;
                r /= 1024;
            } else {
                ss->reduction = 1024;
                r = 1;
            }
            int new_depth = depth + ext;
            int reduced = clampi(new_depth - r, 0, new_depth + 1);
            score = -negamax(t, ss + 1, -alpha - 1, -alpha, reduced, true, false);
            ss->reduction = 1024;
            if (score > alpha && r > 1) {
                bool deeper = score > best_score + DEEPER_BASE + DEEPER_MUL * r;
                bool shallower = score < best_score + new_depth;
                new_depth += deeper - shallower;
                ss->reduction = 1024 * (1 + shallower - deeper);
                if (new_depth - 1 > reduced && score < TB_WIN_BOUND)
                    score = -negamax(t, ss + 1, -alpha - 1, -alpha, new_depth - 1, !cut_node, false);
                ss->reduction = 1024;
                if (quiet && (score <= alpha || score >= beta))
                    update_cont(ss, piece, move_to(m), new_depth, score > alpha);
            } else if (score > alpha && score < best_score + SHALLOWER_MARGIN) {
                new_depth--;
            }
            if (pv_node && score > alpha && score < beta)
                score = -negamax(t, ss + 1, -beta, -alpha, new_depth - 1, false, true);
        }
        undo_move(t, m);
        if (ext >= 2) ss->dext--;

        if (root && t->id == 0 && excluded == NO_MOVE)
            t->root_nodes[from][move_to(m)] += atomic_load_explicit(&t->nodes, memory_order_relaxed) - nodes_before;
        if (stopped()) return 0;

        if (score > best_score) {
            best_score = score;
            if (score > alpha) {
                best_move = m;
                alpha = score;
                alpha_raises++;
                if (pv_node) update_pv(t, ply, m);
            }
            if (alpha >= beta) { ss->cutoffs++; break; }
        }
    }

    if (moves_made == 0) {
        if (excluded != NO_MOVE) return SCORE_NONE;
        return in_check ? mated_in(ply) : 0;
    }

    if (best_score >= beta && !is_decisive(best_score) && !is_decisive(alpha) && !is_decisive(beta))
        best_score = (best_score * depth + beta) / (depth + 1);
    best_score = clampi(best_score, syzygy_min, syzygy_max);

    int bound = best_score >= beta ? BOUND_LOWER : best_score > original_alpha ? BOUND_EXACT : BOUND_UPPER;

    if (best_move != NO_MOVE) {
        if (!move_is_noisy(b, best_move)) {
            ss->killer = best_move;
            int bonus_depth = depth + (!in_check && static_eval <= best_score) + (!root && (ss - 1)->move == NO_MOVE);
            for (int i = 0; i < n_quiets; i++)
                update_quiet_single(t, ss, quiets[i], bonus_depth, quiets[i] == best_move);
        }
        update_tactical(t, ss, tacticals, n_tacticals, best_move, depth);
    }

    if (!root && bound == BOUND_UPPER && (n_quiets > 0 || depth > 3))
        update_inbound(t, ss, hist_bonus(&H_MAIN, depth));

    if (excluded == NO_MOVE) {
        if (raw != SCORE_NONE) {
            int fresh = adjust_eval(t, raw, correction(t, ss));
            if (!in_check && !(best_move != NO_MOVE && move_is_noisy(b, best_move) && see_ge(b, best_move, 0)) &&
                !(bound == BOUND_LOWER && best_score <= fresh) && !(bound == BOUND_UPPER && best_score >= fresh))
                update_correction(t, ss, depth, complexity, best_score - fresh);
        }
        tt_store(b->hash, ply, best_move, best_score, raw, bound, depth, ss->ttpv);
    }
    return best_score;
}

/* ------------------------------------------------------------------ */
/* Iterative deepening. */

static void print_score(char *buf, size_t n, int score) {
    if (score >= MATE_BOUND) snprintf(buf, n, "mate %d", (MATE_SCORE - score + 1) / 2);
    else if (score <= -MATE_BOUND) snprintf(buf, n, "mate -%d", (MATE_SCORE + score) / 2);
    else snprintf(buf, n, "cp %d", score);
}

static void report(ThreadData *t, int depth, int score, int bound, const Move *pv, int pv_len) {
    if (!search_verbose) return;
    int64_t ms = elapsed_ms();
    uint64_t nodes = total_nodes();
    char sc[32];
    print_score(sc, sizeof sc, score);
    printf("info depth %d seldepth %d score %s%s nodes %llu nps %llu hashfull %d tbhits %llu time %lld pv",
           depth, t->seldepth, sc,
           bound == BOUND_LOWER ? " lowerbound" : bound == BOUND_UPPER ? " upperbound" : "",
           (unsigned long long)nodes, (unsigned long long)(ms > 0 ? nodes * 1000 / (uint64_t)ms : nodes),
           tt_hashfull(), (unsigned long long)total_tbhits(), (long long)ms);
    Board pvb = t->board;
    for (int i = 0; i < pv_len; i++) {
        if (!move_is_pseudo_legal(&pvb, pv[i]) || !move_is_legal(&pvb, pv[i])) break;
        printf(" %s", move_to_uci(&pvb, pv[i]));
        make_move(&pvb, pv[i]);
        pvb.ply = 0;
    }
    printf("\n");
    fflush(stdout);
}

static void iterative_deepening(ThreadData *t) {
    Board *b = &t->board;
    int max_depth = limits.depth > 0 ? (limits.depth < MAX_PLY - 1 ? limits.depth : MAX_PLY - 1) : MAX_PLY - 1;
    int average = SCORE_NONE;
    Stack *ss = &t->stack[STACK_OFFSET];
    int mate_hits = 0;

    for (int depth = 1; depth <= max_depth; depth++) {
        t->root_depth = depth;
        t->seldepth = 0;
        t->optimism[0] = t->optimism[1] = 0;
        int alpha = -INFINITY_SCORE, beta = INFINITY_SCORE, delta = ASP_DELTA, fail_high = 0;
        if (depth > 1 && average != SCORE_NONE) {
            t->optimism[b->side] = 128 * average / (abs(average) + OPTIMISM_OFFSET);
            t->optimism[b->side ^ 1] = -t->optimism[b->side];
            delta += average * average / ASP_DIVISOR;
            alpha = clampi(average - delta, -INFINITY_SCORE, INFINITY_SCORE);
            beta = clampi(average + delta, -INFINITY_SCORE, INFINITY_SCORE);
        }
        int score;
        for (;;) {
            int draft = depth - fail_high;
            if (draft < (depth + 1) / 2) draft = (depth + 1) / 2;
            if (draft < 1) draft = 1;
            score = negamax(t, ss, alpha, beta, draft, false, true);
            if (stopped()) break;
            if (score <= alpha) {
                beta = (alpha + beta) / 2;
                alpha = clampi(score - delta, -INFINITY_SCORE, INFINITY_SCORE);
                fail_high = 0;
                if (t->id == 0) {
                    if (time_managed && tm_failed_low < 2) { tm_failed_low++; tm_rescale(-1); }
                    if (elapsed_ms() > 2000) report(t, depth, score, BOUND_UPPER, t->best_pv, t->best_pv_len);
                }
            } else if (score >= beta) {
                beta = clampi(score + delta, -INFINITY_SCORE, INFINITY_SCORE);
                if (!is_decisive(score)) fail_high++;
                if (t->pv_len[0] > 0) {
                    memcpy(t->best_pv, t->pv[0], (size_t)t->pv_len[0] * sizeof(Move));
                    t->best_pv_len = t->pv_len[0];
                }
                if (t->id == 0 && elapsed_ms() > 2000) report(t, depth, score, BOUND_LOWER, t->best_pv, t->best_pv_len);
            } else {
                if (t->pv_len[0] > 0) {
                    memcpy(t->best_pv, t->pv[0], (size_t)t->pv_len[0] * sizeof(Move));
                    t->best_pv_len = t->pv_len[0];
                }
                break;
            }
            delta += delta * (ASP_GROWTH + 19 * fail_high) / 128;
        }
        if (stopped()) break;

        t->completed = depth;
        t->score = score;
        average = average == SCORE_NONE ? score : (2 * score + average) / 3;

        if (t->id != 0) continue;
        report(t, depth, score, BOUND_EXACT, t->best_pv, t->best_pv_len);

        Move best = t->best_pv_len ? t->best_pv[0] : NO_MOVE;
        /* A best move that beats every alternative by a wide margin needs less time. */
        if (time_managed && tm_forced == 1.0 && depth >= 8 && best != NO_MOVE && !is_decisive(score)) {
            int margin = depth >= 12 ? 170 : 400;
            int r_beta = clampi(score - margin, -TB_WIN_BOUND + 1, TB_WIN_BOUND - 1);
            int r_depth = ((depth - 1) / 2 < 12 ? (depth - 1) / 2 : 12) - 1;
            int saved_seldepth = t->seldepth;
            ss->excluded = best;
            int v = negamax(t, ss, r_beta - 1, r_beta, r_depth > 1 ? r_depth : 1, false, false);
            ss->excluded = NO_MOVE;
            t->seldepth = saved_seldepth;
            if (stopped()) break;
            if (v < r_beta) tm_forced = depth >= 12 ? 0.627 : 0.386;
        }
        if (time_managed && depth > 4) {
            tm_stability = best == tm_prev_best ? tm_stability + 1 : 0;
            double frac = -1;
            if (depth > 8 && best != NO_MOVE) {
                uint64_t n = atomic_load_explicit(&t->nodes, memory_order_relaxed);
                frac = n ? (double)t->root_nodes[move_from(best)][move_to(best)] / (double)n : -1;
            }
            tm_rescale(frac);
        }
        tm_prev_best = best;

        if (limits.mate > 0 && score >= MATE_BOUND && (MATE_SCORE - score + 1) / 2 <= limits.mate) break;
        if (time_managed && depth > 10 && abs(score) >= MATE_BOUND) {
            if (++mate_hits >= 3) break;
        } else mate_hits = 0;
        if (!atomic_load(&pondering) && elapsed_ms() >= opt_time) break;
    }
}

static void *thread_main(void *arg) {
    ThreadData *t = arg;
    nnue_prepare_search(&t->board);   /* network state is thread-local */
    iterative_deepening(t);
    return NULL;
}

/* Thread voting: weight each thread's move by depth and relative score. */
static ThreadData *select_best_thread(void) {
    ThreadData *best = threads[0];
    if (thread_count == 1) return best;
    int min_score = INFINITY_SCORE;
    for (int i = 0; i < thread_count; i++)
        if (threads[i]->completed > 0 && threads[i]->score < min_score) min_score = threads[i]->score;
    int64_t votes[64 * 64];
    memset(votes, 0, sizeof votes);
    for (int i = 0; i < thread_count; i++) {
        ThreadData *t = threads[i];
        if (!t->completed || !t->best_pv_len) continue;
        Move m = t->best_pv[0];
        votes[move_from(m) * 64 + move_to(m)] += (int64_t)(t->score - min_score + 10) * t->completed;
    }
    for (int i = 1; i < thread_count; i++) {
        ThreadData *t = threads[i];
        if (!t->completed || !t->best_pv_len) continue;
        if (!best->best_pv_len) { best = t; continue; }
        if (is_decisive(best->score)) {
            if (abs(t->score) > abs(best->score)) best = t;
            continue;
        }
        if (t->score >= TB_WIN_BOUND) { best = t; continue; }
        Move bm = best->best_pv[0], tm = t->best_pv[0];
        int64_t bv = votes[move_from(bm) * 64 + move_to(bm)], tv = votes[move_from(tm) * 64 + move_to(tm)];
        if (tv > bv || (tv == bv && (int64_t)(t->score - min_score + 10) * t->completed >
                                    (int64_t)(best->score - min_score + 10) * best->completed))
            best = t;
    }
    return best;
}

static void reset_thread(ThreadData *t, const Board *root, const uint64_t *history, int history_len) {
    t->board = *root;
    t->board.ply = 0;
    atomic_store(&t->nodes, 0);
    t->tbhits = 0;
    t->seldepth = t->root_depth = t->completed = 0;
    t->score = -INFINITY_SCORE;
    t->best_pv_len = 0;
    t->nmp_banned[0] = t->nmp_banned[1] = false;
    memset(t->root_nodes, 0, sizeof t->root_nodes);
    memset(t->stack, 0, sizeof t->stack);
    memset(t->pv_len, 0, sizeof t->pv_len);
    for (int i = 0; i < MAX_PLY + STACK_OFFSET + 4; i++) {
        t->stack[i].piece = -1;
        t->stack[i].static_eval = t->stack[i].eval = SCORE_NONE;
        t->stack[i].ply = i - STACK_OFFSET;
        t->stack[i].plies_from_null = root->half_move_clock + MAX_PLY;
    }
    int n = history_len > KEY_HISTORY - 1 ? KEY_HISTORY - 1 : history_len;
    if (n > 0) memcpy(t->keys, history + (history_len - n), (size_t)n * sizeof(uint64_t));
    t->key_root = n;
    t->keys[n] = root->hash;
}

static int legal_moves(Board *b, Move *out) {
    MoveList ml;
    generate_moves(b, &ml);
    int n = 0;
    for (int i = 0; i < ml.count; i++)
        if (move_is_legal(b, ml.moves[i])) out[n++] = ml.moves[i];
    return n;
}

void search_run(const Board *root, const uint64_t *history, int history_len,
                const SearchLimits *lim, SearchResult *result) {
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    limits = *lim;
    atomic_store(&pondering, lim->ponder);
    memset(result, 0, sizeof *result);
    if (!threads) search_set_threads(1);

    Board rb = *root;
    rb.ply = 0;
    Move legal[MAX_MOVES];
    int n_legal = legal_moves(&rb, legal);
    setup_time(&rb);
    tt_new_search();

    if (n_legal == 0) {
        if (search_verbose) {
            printf("info depth 0 score %s\nbestmove 0000\n", rb.checkers ? "mate 0" : "cp 0");
            fflush(stdout);
        }
        atomic_store(&stop_flag, false);
        return;
    }

    /* Tablebase root: play the DTZ-optimal move under a real clock. */
    if (time_managed && !lim->ponder && syzygy_available()) {
        Move tb = syzygy_probe_root(&rb);
        if (tb != NO_MOVE) {
            result->best = tb;
            if (search_verbose) {
                printf("info depth 1 string syzygy root move\nbestmove %s\n", move_to_uci(&rb, tb));
                fflush(stdout);
            }
            atomic_store(&stop_flag, false);
            return;
        }
    }
    if (n_legal == 1 && time_managed) { opt_time = 0; base_opt = 0; }

    for (int i = 0; i < thread_count; i++) reset_thread(threads[i], &rb, history, history_len);

    pthread_t handles[thread_count > 1 ? thread_count : 1];
    int launched = 1;
    for (int i = 1; i < thread_count; i++) {
        if (pthread_create(&handles[i], NULL, thread_main, threads[i])) break;
        launched++;
    }
    thread_main(threads[0]);

    /* UCI: never answer before "stop"/"ponderhit" in infinite or ponder mode. */
    pthread_mutex_lock(&wake_mutex);
    while (!stopped() && (atomic_load(&pondering) || limits.infinite))
        pthread_cond_wait(&wake_cond, &wake_mutex);
    pthread_mutex_unlock(&wake_mutex);
    atomic_store(&stop_flag, true);
    for (int i = 1; i < launched; i++) pthread_join(handles[i], NULL);

    /* Stopped before the first iteration finished: still answer with a searched move. */
    if (!threads[0]->best_pv_len) {
        ThreadData *t = threads[0];
        atomic_store(&stop_flag, false);
        negamax(t, &t->stack[STACK_OFFSET], -INFINITY_SCORE, INFINITY_SCORE, 1, false, true);
        if (t->pv_len[0] > 0) {
            t->best_pv[0] = t->pv[0][0];
            t->best_pv_len = 1;
        }
        atomic_store(&stop_flag, true);
    }

    ThreadData *best = select_best_thread();
    if (best != threads[0] && best->best_pv_len)
        report(best, best->completed, best->score, BOUND_EXACT, best->best_pv, best->best_pv_len);

    Move bm = best->best_pv_len ? validated(&rb, best->best_pv[0]) : NO_MOVE;
    if (bm == NO_MOVE) bm = legal[0];
    Move pm = NO_MOVE;
    {
        Board after = rb;
        make_move(&after, bm);
        after.ply = 0;
        if (best->best_pv_len > 1 && bm == best->best_pv[0]) pm = validated(&after, best->best_pv[1]);
        if (pm == NO_MOVE) pm = validated(&after, tt_probe_move(after.hash));
        result->best = bm;
        result->ponder = pm;
        result->score = best->score;
        result->depth = best->completed;
        result->nodes = total_nodes();
        result->time_ms = (int)elapsed_ms();
        if (search_verbose) {
            printf("bestmove %s", move_to_uci(&rb, bm));
            if (pm != NO_MOVE) printf(" ponder %s", move_to_uci(&after, pm));
            printf("\n");
            fflush(stdout);
        }
    }
    atomic_store(&stop_flag, false);
}

void search_stop(void) {
    pthread_mutex_lock(&wake_mutex);
    atomic_store(&pondering, false);
    atomic_store(&stop_flag, true);
    pthread_cond_broadcast(&wake_cond);
    pthread_mutex_unlock(&wake_mutex);
}

void search_ponderhit(void) {
    pthread_mutex_lock(&wake_mutex);
    atomic_store(&pondering, false);
    pthread_cond_broadcast(&wake_cond);
    pthread_mutex_unlock(&wake_mutex);
}

/* ------------------------------------------------------------------ */
/* Setup. */

static void clear_thread(ThreadData *t) {
    memset(t->from_to, 0, sizeof t->from_to);
    memset(t->piece_to, 0, sizeof t->piece_to);
    memset(t->tactical, 0, sizeof t->tactical);
    memset(t->cont, 0, sizeof t->cont);
    memset(t->pawn_hist, 0, sizeof t->pawn_hist);
    memset(t->corr_pawn, 0, sizeof t->corr_pawn);
    memset(t->corr_nonpawn, 0, sizeof t->corr_nonpawn);
    memset(t->corr_minor, 0, sizeof t->corr_minor);
    memset(t->corr_cont, 0, sizeof t->corr_cont);
}

void search_clear(void) {
    for (int i = 0; i < thread_count; i++) clear_thread(threads[i]);
    eval_clear_pawn_hash();
}

void search_set_threads(int n) {
    if (n < 1) n = 1;
    for (int i = 0; i < thread_count; i++) rv_aligned_free(threads[i]);
    free(threads);
    threads = calloc((size_t)n, sizeof *threads);
    thread_count = 0;
    if (!threads) { fprintf(stderr, "thread allocation failed\n"); exit(1); }
    for (int i = 0; i < n; i++) {
        ThreadData *t = rv_aligned_alloc(64, sizeof(ThreadData));
        if (!t) {
            if (i == 0) { fprintf(stderr, "thread allocation failed\n"); exit(1); }
            fprintf(stderr, "info string only %d threads could be allocated\n", i);
            break;
        }
        memset(t, 0, sizeof *t);
        t->id = i;
        threads[i] = t;
        thread_count++;
    }
}

int search_get_threads(void) { return thread_count; }

void search_init(void) {
    for (int d = 0; d < 64; d++)
        for (int m = 0; m < 64; m++)
            lmr_table[d][m] = (d && m) ? (int)((0.99 + log((double)d) * log((double)m) / 2.60) * 1024.0) : 0;
    for (int d = 0; d < 12; d++) {
        lmp_table[0][d] = (int)(2.5 + 2.0 * d * d / 4.5);
        lmp_table[1][d] = (int)(4.0 + 4.0 * d * d / 4.5);
    }
    uint64_t s = 0x2545F4914F6CDD1DULL;
    for (int p = 0; p < 13; p++)
        for (int sq = 0; sq < 64; sq++) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            cont_keys[p][sq] = s;
        }
}
