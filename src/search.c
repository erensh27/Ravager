/* search.c — the Ravager 2.0 search (Phase A revision).
 *
 * Techniques assembled from the classic pure-HCE engines (full credits in
 * docs/CREDITS.md): principal variation search with aspiration windows, iterative
 * deepening, transposition-table move ordering + cutoffs, killers, countermove
 * heuristics, butterfly + continuation history with gravity-style
 * bonus/malus, late move reductions, late move pruning, reverse futility
 * pruning, razoring, futility pruning, adaptive null-move pruning with
 * verification, internal iterative reductions, exclusion-based singular
 * extensions, check extensions, SEE-pruned quiescence with check evasions.
 */

#define _POSIX_C_SOURCE 200809L

#include "bitboard.h"
#include "tt.h"
#include "search.h"
#include "nnue.h"
#include "tb_syzygy.h"
#include <pthread.h>
#include <stdatomic.h>

volatile int search_soft_ms  = 1000;
volatile int search_hard_ms  = 5000;
int search_max_depth = 100;
int search_verbose = 1;
_Thread_local uint64_t search_nodes = 0;
_Thread_local Move search_root_best = NO_MOVE;
_Thread_local uint64_t root_best_nodes = 0;

bool is_insufficient_material(const Board *b); /* evaluate.c */
void eval_clear_pawn_hash(void);               /* evaluate.c */

static atomic_bool abort_search = false;
int search_threads = 1;
static _Thread_local int worker_id;
static _Thread_local bool worker_quiet;
static _Atomic uint64_t *node_counts;
static int active_workers;
void search_new_job(void) { atomic_store(&abort_search,false); }
static uint64_t total_nodes(void) {
    if(!node_counts) return search_nodes;
    atomic_store_explicit(&node_counts[worker_id],search_nodes,memory_order_relaxed);
    uint64_t n=0;
    for(int i=0;i<active_workers;i++) n+=atomic_load_explicit(&node_counts[i],memory_order_relaxed);
    return n;
}
static _Thread_local struct timespec search_start_ts;
static _Thread_local int seldepth = 0;
static _Thread_local int root_best_score = 0;

/* Game history for repetition across search root */
static uint64_t game_history[2048];
static int game_history_len = 0;
void search_set_game_history(const uint64_t *h, int len) {
    game_history_len = len < 2048 ? len : 2048;
    for (int i = 0; i < game_history_len; i++) game_history[i] = h[i];
}

static inline int elapsed_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int)((now.tv_sec - search_start_ts.tv_sec) * 1000 +
                 (now.tv_nsec - search_start_ts.tv_nsec) / 1000000);
}

static inline void check_time(void) {
    if (abort_search) return;
    if ((search_nodes & 1023) == 0) total_nodes();
    if ((search_nodes & 1023) == 0 && elapsed_ms() >= search_hard_ms)
        abort_search = true;
}

/* ---- History heuristics ----
 * Butterfly history:      [side][from][to]
 * Continuation history:   cmh_table[CTX_COUNTER] keyed by the opponent's
 *                         previous move, cmh_table[CTX_FOLLOW] keyed by our
 *                         own previous move (two plies back). Both map
 *                         [prev_piece][prev_to][curr_piece][curr_to].
 * cm_piece/cm_to stack tracks the move that led into each ply so the
 * contexts are available without board lookups. */
#define CTX_COUNTER 0
#define CTX_FOLLOW   1
#define CTX_FOLLOW4  2

static _Thread_local int history_table[2][64][64];
static _Thread_local int pawn_history_table[2][2048][64];
static _Thread_local int cmh_table[3][6][64][6][64];
static _Thread_local Move killers[MAX_PLY][2];
static _Thread_local Move counter_moves[2][64][64];
static _Thread_local int8_t  cm_piece[MAX_PLY + 8];
static _Thread_local uint8_t cm_to[MAX_PLY + 8];
static _Thread_local int     eval_stack[MAX_PLY + 8];   /* static evals by ply, for `improving` */

/* Capture history: [side][attacker_piece][to_sq][captured_piece] */
static _Thread_local int capture_history[2][6][64][6];

static void update_history_entry(int *h, int bonus) {
    /* gravity formula, Ethereal/Stockfish style */
    *h += bonus - *h * abs(bonus) / 16384;
}

static void update_histories(Board *b, int ply, Move best, int depth,
                             Move *searched_quiets, int n_quiets, Move prev_move) {
    int side = b->side;               /* mover — called after unmake */
    /* Clamped bonus: faster convergence at shallow depth, avoids overflow */
    int bonus = 32 * depth * depth + 64 * depth - 64;
    if (bonus > 2048) bonus = 2048;
    if (bonus < 0) bonus = 0;

    if (killers[ply][0] != best) {
        killers[ply][1] = killers[ply][0];
        killers[ply][0] = best;
    }
    if (prev_move != NO_MOVE)
        counter_moves[side][move_from(prev_move)][move_to(prev_move)] = best;

    /* continuation contexts for this ply */
    int p1 = (ply >= 1) ? cm_piece[ply]         : -1;
    int t1 = (ply >= 1) ? cm_to[ply]            :  0;
    int p2 = (ply >= 2) ? cm_piece[ply - 1]     : -1;
    int t2 = (ply >= 2) ? cm_to[ply - 1]        :  0;
    int p4 = (ply >= 4) ? cm_piece[ply - 3]     : -1;
    int t4 = (ply >= 4) ? cm_to[ply - 3]        :  0;

    int bf = move_from(best), bt = move_to(best);
    int bp = b->piece_on[bf] % 6;
    update_history_entry(&history_table[side][bf][bt], bonus);
    if (bp == PAWN) update_history_entry(&pawn_history_table[side][(b->pieces[WHITE][PAWN] ^ b->pieces[BLACK][PAWN]) % 2048][bt], bonus);
    if (p1 >= 0) update_history_entry(&cmh_table[CTX_COUNTER][p1][t1][bp][bt], bonus);
    if (p2 >= 0) update_history_entry(&cmh_table[CTX_FOLLOW][p2][t2][bp][bt], bonus);
    if (p4 >= 0) update_history_entry(&cmh_table[CTX_FOLLOW4][p4][t4][bp][bt], bonus);

    for (int i = 0; i < n_quiets; i++) {
        Move m = searched_quiets[i];
        int f = move_from(m), t = move_to(m);
        int pt = b->piece_on[f] % 6;
        update_history_entry(&history_table[side][f][t], -bonus);
        if (pt == PAWN) update_history_entry(&pawn_history_table[side][(b->pieces[WHITE][PAWN] ^ b->pieces[BLACK][PAWN]) % 2048][t], -bonus);
        if (p1 >= 0) update_history_entry(&cmh_table[CTX_COUNTER][p1][t1][pt][t], -bonus);
        if (p2 >= 0) update_history_entry(&cmh_table[CTX_FOLLOW][p2][t2][pt][t], -bonus);
        if (p4 >= 0) update_history_entry(&cmh_table[CTX_FOLLOW4][p4][t4][pt][t], -bonus);
    }
}

static inline void update_capture_hist(int side, int attacker, int to, int victim, int bonus) {
    int *h = &capture_history[side][attacker][to][victim];
    *h += bonus - *h * abs(bonus) / 16384;
}

static void clear_search_tables(void) {
    memset(history_table, 0, sizeof(history_table));
    memset(pawn_history_table, 0, sizeof(pawn_history_table));
    memset(cmh_table, 0, sizeof(cmh_table));
    memset(killers, 0, sizeof(killers));
    memset(counter_moves, 0, sizeof(counter_moves));
    memset(cm_piece, -1, sizeof(cm_piece));
    memset(cm_to, 0, sizeof(cm_to));
    memset(capture_history, 0, sizeof(capture_history));
}

static void age_history_tables(void) {
    for (int c = 0; c < 2; c++)
        for (int f = 0; f < 64; f++)
            for (int t = 0; t < 64; t++)
                history_table[c][f][t] /= 2;
    for (int ctx = 0; ctx < 3; ctx++)
        for (int a = 0; a < 6; a++)
            for (int b2 = 0; b2 < 64; b2++)
                for (int c2 = 0; c2 < 6; c2++)
                    for (int d = 0; d < 64; d++)
                        cmh_table[ctx][a][b2][c2][d] /= 2;
    for (int c = 0; c < 2; c++)
        for (int a = 0; a < 6; a++)
            for (int t = 0; t < 64; t++)
                for (int v = 0; v < 6; v++)
                    capture_history[c][a][t][v] /= 2;
    for (int ply = MAX_PLY - 1; ply >= 2; ply--) {
        killers[ply][0] = killers[ply - 2][0];
        killers[ply][1] = killers[ply - 2][1];
    }
    killers[0][0] = killers[0][1] = NO_MOVE;
    killers[1][0] = killers[1][1] = NO_MOVE;
}

/* ---- Move ordering ---- */
static const int mvv_victim[6] = { 100, 320, 330, 500, 900, 20000 };
static const int mvv_attacker[6] = { 1, 2, 3, 4, 5, 6 };

static inline bool is_capture(const Board *b, Move m) {
    return !move_is_castle(m) && ((b->occ_all & (1ULL << move_to(m))) || move_is_ep(m));
}

static void score_moves(Board *b, MoveList *ml, Move tt_move, int ply, Move prev_move) {
    int side = b->side;
    Move cm = NO_MOVE;
    if (prev_move != NO_MOVE)
        cm = counter_moves[side][move_from(prev_move)][move_to(prev_move)];

    int p1 = (ply >= 1) ? cm_piece[ply]     : -1;
    int t1 = (ply >= 1) ? cm_to[ply]        :  0;
    int p2 = (ply >= 2) ? cm_piece[ply - 1] : -1;
    int t2 = (ply >= 2) ? cm_to[ply - 1]    :  0;
    int p4 = (ply >= 4) ? cm_piece[ply - 3] : -1;
    int t4 = (ply >= 4) ? cm_to[ply - 3]    :  0;

    for (int i = 0; i < ml->count; i++) {
        Move m = ml->moves[i];
        int score;

        ml->see_checked[i] = true;

        if (m == tt_move) score = 1 << 24;
        else if (move_is_promo(m) && move_promo(m) == PROMO_QUEEN) score = (1 << 23) + 1000;
        else if (is_capture(b, m)) {
            int victim   = (b->occ_all & (1ULL << move_to(m))) ? (b->piece_on[move_to(m)] % 6) : PAWN;
            int attacker = b->piece_on[move_from(m)] % 6;
            ml->see_checked[i] = false;
            /* Optimistically score captures without SEE */
            score = (1 << 22) + mvv_victim[victim] * 16 - mvv_attacker[attacker]
                  + capture_history[side][attacker][move_to(m)][victim] / 16;
        }
        else if (ply < MAX_PLY && m == killers[ply][0]) score = (1 << 21);
        else if (ply < MAX_PLY && m == killers[ply][1]) score = (1 << 21) - 1;
        else if (m == cm) score = (1 << 20);
        else {
            int f = move_from(m), t = move_to(m);
            int pt = b->piece_on[f] % 6;
            score = history_table[side][f][t];
            if (pt == PAWN) score += pawn_history_table[side][(b->pieces[WHITE][PAWN] ^ b->pieces[BLACK][PAWN]) % 2048][t];
            if (p1 >= 0) score += cmh_table[CTX_COUNTER][p1][t1][pt][t];
            if (p2 >= 0) score += cmh_table[CTX_FOLLOW][p2][t2][pt][t];
            if (p4 >= 0) score += cmh_table[CTX_FOLLOW4][p4][t4][pt][t];
        }
        ml->scores[i] = score;
    }
}

static Move pick_next_move(Board *b, MoveList *ml, int idx) {
    while(true) {
    int best = idx;
    for (int i = idx + 1; i < ml->count; i++)
        if (ml->scores[i] > ml->scores[best]) best = i;
    if (best != idx) {
        Move tm = ml->moves[idx]; ml->moves[idx] = ml->moves[best]; ml->moves[best] = tm;
        int  ts = ml->scores[idx]; ml->scores[idx] = ml->scores[best]; ml->scores[best] = ts;
        bool tc = ml->see_checked[idx]; ml->see_checked[idx] = ml->see_checked[best]; ml->see_checked[best] = tc;
    }
    Move m = ml->moves[idx];
    if (!ml->see_checked[idx]) {
        ml->see_checked[idx] = true;
        int see_val = see_move(b, m);
        if (see_val < 0) {
            ml->scores[idx] = (1 << 19) + see_val;
            continue;
        } else {
            ml->scores[idx] += see_val * 4;
        }
    }
    return m;
    }
}

/* ---- LMR table (log-log reductions, Stockfish tradition) ---- */
static int lmr_table[64][64];
void init_lmr_table(void) {
    for (int d = 1; d < 64; d++)
        for (int m = 1; m < 64; m++)
            lmr_table[d][m] = (int)(0.75f + logf((float)d) * logf((float)m) / 2.25f);
}

/* ---- Draw detection ---- */
static bool is_repetition(const Board *b) {
    /* Search stack: the undo stack holds the pre-move hashes. */
    for (int i = b->ply - 2; i >= 0; i -= 2) {
        if (b->undo_stack[i].half_move_clock == 0) break;
        if (b->undo_stack[i].zobrist_key == b->hash) return true;
    }
    /* Game history: the root hash sits at game_history[len-1]; a position
     * ply plies into the search repeats at indices len-1-ply, len-3-ply, ...
     * and only within the reversible (halfmove-clock) stretch. */
    int start = game_history_len - 1 - b->ply;
    for (int i = start - 2; i >= 0; i -= 2) {
        if (start - i > b->half_move_clock + 2) break;
        if (game_history[i] == b->hash) return true;
    }
    return false;
}

static bool is_dead_draw(const Board *b) {
    return is_insufficient_material(b) || b->half_move_clock >= 100;
}

/* ---- Quiescence ---- */
static int quiescence(Board *b, int alpha, int beta, int ply) {
    if (abort_search) return 0;
    if (ply >= MAX_PLY - 1) return nnue_eval(b);

    search_nodes++;
    if (ply > seldepth) seldepth = ply;

    if (is_dead_draw(b)) return 0;

    int tt_score = 0;
    Move tt_move = NO_MOVE;
    int tt_depth = -1, tt_bound = BOUND_UPPER;
    bool tt_hit = tt_probe(b->hash, &tt_score, &tt_move, &tt_depth, &tt_bound, ply);
    if (tt_hit) {
        if (tt_bound == BOUND_EXACT)                      return tt_score;
        if (tt_bound == BOUND_LOWER && tt_score >= beta)  return tt_score;
        if (tt_bound == BOUND_UPPER && tt_score <= alpha) return tt_score;
    }

    bool in_check = is_in_check(b, b->side);
    int mover = b->side;
    int original_alpha = alpha;
    int best_score = -INFINITY_SCORE;
    Move best_move = NO_MOVE;

    if (!in_check) {
        int stand_pat = nnue_eval(b);
        if (stand_pat >= beta) {
            tt_store(b->hash, stand_pat, NO_MOVE, 0, BOUND_LOWER, ply);
            return beta;
        }
        if (stand_pat > alpha) alpha = stand_pat;
        best_score = stand_pat;
        if (stand_pat < alpha - 1000) return alpha;   /* delta pruning */

        MoveList caps;
        generate_captures(b, &caps);
        score_moves(b, &caps, tt_move, ply, NO_MOVE);

        for (int i = 0; i < caps.count; i++) {
            Move m = pick_next_move(b, &caps, i);
            /* skip clearly losing captures */
            if (!move_is_promo(m) && see_move(b, m) < -50) continue;
            nnue_push_move(b, m);
            make_move(b, m);
            if (is_in_check(b, mover)) { unmake_move(b, m); continue; }
            int score = -quiescence(b, -beta, -alpha, ply + 1);
            unmake_move(b, m);
            if (abort_search) return 0;
            if (score > best_score) {
                best_score = score;
                best_move = m;
            }
            if (score >= beta) {
                tt_store(b->hash, score, m, 0, BOUND_LOWER, ply);
                return beta;
            }
            if (score > alpha) alpha = score;
        }
        tt_store(b->hash, best_score, best_move, 0,
                 best_score > original_alpha ? BOUND_EXACT : BOUND_UPPER, ply);
        return alpha;
    }

    /* In check: full evasion search (legal evasion generator) */
    MoveList moves;
    generate_evasions(b, &moves);
    if (moves.count == 0) return -MATE_SCORE + ply;
    score_moves(b, &moves, tt_move, ply, NO_MOVE);
    for (int i = 0; i < moves.count; i++) {
        Move m = pick_next_move(b, &moves, i);
        nnue_push_move(b, m);
        make_move(b, m);
        int score = -quiescence(b, -beta, -alpha, ply + 1);
        unmake_move(b, m);
        if (abort_search) return 0;
        if (score > best_score) {
            best_score = score;
            best_move = m;
        }
        if (score >= beta) {
            tt_store(b->hash, score, m, 0, BOUND_LOWER, ply);
            return beta;
        }
        if (score > alpha) alpha = score;
    }
    tt_store(b->hash, best_score, best_move, 0,
             best_score > original_alpha ? BOUND_EXACT : BOUND_UPPER, ply);
    return alpha;
}

/* ---- Main PVS / negamax ----
 * `excluded` is the move skipped by the singular-extension verification
 * search; when set, the TT is neither probed nor stored (the hash key does
 * not encode the exclusion). */
static int pvs(Board *b, int alpha, int beta, int depth, int ply, bool is_pv,
               Move prev_move, bool null_ok, Move excluded) {
    if (abort_search) return 0;
    if (ply >= MAX_PLY - 1) return nnue_eval(b);

    bool in_check = is_in_check(b, b->side);
    if (in_check && depth < MAX_PLY - 3) {
        depth++;                                    /* check extension */
        int ksq = lsb(b->pieces[b->side][KING]);
        if (popcount(all_attackers_to(b, ksq, b->occ_all) & b->occupancy[b->side ^ 1]) >= 2)
            depth++;                                /* double-check extension */
    }

    if (depth <= 0) return quiescence(b, alpha, beta, ply);

    search_nodes++;
    check_time();
    if (ply > seldepth) seldepth = ply;

    if (ply > 0) {
        if (is_dead_draw(b)) return 0;
        if (is_repetition(b)) return 0;
        /* mate-distance pruning */
        int mate_alpha = alpha > -MATE_SCORE + ply ? alpha : -MATE_SCORE + ply;
        int mate_beta  = beta  <  MATE_SCORE - ply - 1 ? beta : MATE_SCORE - ply - 1;
        if (mate_alpha >= mate_beta) return mate_alpha;
        alpha = mate_alpha; beta = mate_beta;

        /* Syzygy WDL: exact result known, no search needed */
        int tb_score;
        if (syzygy_probe_wdl(b, ply, &tb_score)) return tb_score;
    }

    bool root = (ply == 0);

    /* TT probe (skipped during singular verification) */
    int tt_score = 0;
    Move tt_move = NO_MOVE;
    int tt_depth = -1, tt_bound = BOUND_UPPER;
    bool tt_hit = false;
    if (excluded == NO_MOVE)
        tt_hit = tt_probe(b->hash, &tt_score, &tt_move, &tt_depth, &tt_bound, ply);

    if (tt_hit && !is_pv && !root && tt_depth >= depth) {
        if (tt_bound == BOUND_EXACT)                      return tt_score;
        if (tt_bound == BOUND_LOWER && tt_score >= beta)  return tt_score;
        if (tt_bound == BOUND_UPPER && tt_score <= alpha) return tt_score;
    }

    /* Internal iterative reduction: no TT move, or TT move has stale depth */
    if ((!tt_hit || tt_depth < depth - 4) && depth >= 4 && !root && excluded == NO_MOVE)
        depth -= 1;

    int static_eval = in_check ? -INFINITY_SCORE : nnue_eval(b);
    if (!in_check) eval_stack[ply] = static_eval;
    else eval_stack[ply] = -INFINITY_SCORE;
    /* improving: our eval is better than two plies ago (same side to move) */
    bool improving = !in_check && ply >= 2 &&
                     eval_stack[ply - 2] > -INFINITY_SCORE &&
                     static_eval > eval_stack[ply - 2];

    /* Reverse futility pruning (static null move), improving-scaled margin */
    if (!is_pv && !in_check && depth <= 8) {
        int margin = (improving ? 78 : 102) * depth;
        if (static_eval - margin >= beta) return static_eval;
    }

    /* Razoring: hopeless nodes drop straight into qsearch */
    if (!is_pv && !in_check && depth <= 3) {
        int razor_margin = 250 + 120 * depth;
        if (static_eval + razor_margin < alpha) {
            int q = quiescence(b, alpha, beta, ply);
            if (q < alpha) return q;
        }
    }

    /* Null move pruning with adaptive R and verification at high depth */
    if (!is_pv && !in_check && null_ok && depth >= 3 && static_eval >= beta) {
        Bitboard non_pawn = b->occupancy[b->side] & ~b->pieces[b->side][PAWN] & ~b->pieces[b->side][KING];
        if (non_pawn) {
            int R = 3 + depth / 4 + (improving ? 0 : 1);
            if (static_eval - beta > 300) R++;
            if (R > depth - 1) R = depth - 1;
            if (R < 1) R = 1;
            cm_piece[ply + 1] = -1;                 /* no continuation after null */
            eval_stack[ply + 1] = -INFINITY_SCORE;
            nnue_push_null(b);
            make_null_move(b);
            int null_score = -pvs(b, -beta, -beta + 1, depth - 1 - R, ply + 1, false, NO_MOVE, false, NO_MOVE);
            unmake_null_move(b);
            if (abort_search) return 0;
            if (null_score >= beta) {
                if (depth < 12) return beta;
                /* verification search */
                int verify = pvs(b, alpha, beta, depth - 1 - R, ply, false, prev_move, false, NO_MOVE);
                if (verify >= beta) return beta;
            }
        }
    }

    /* ProbCut-lite: only for nodes already near beta, and skipped when the
     * TT already suggests the node fails low */
    if (!is_pv && !in_check && depth >= 5 && static_eval >= beta - 250 &&
        !(tt_hit && tt_bound == BOUND_UPPER && tt_score < beta - 250)) {
        int pc_beta = beta + 180;
        MoveList caps;
        generate_captures(b, &caps);
        score_moves(b, &caps, NO_MOVE, ply, NO_MOVE);
        int tried = 0;
        for (int i = 0; i < caps.count && tried < 3; i++) {
            Move m = pick_next_move(b, &caps, i);
            if (see_move(b, m) < 120) continue;
            int mp = b->piece_on[move_from(m)] % 6;
            nnue_push_move(b, m);
            make_move(b, m);
            if (is_in_check(b, b->side ^ 1)) { unmake_move(b, m); continue; }  /* illegal */
            if (is_in_check(b, b->side))     { unmake_move(b, m); continue; }  /* gives check: skip */
            cm_piece[ply + 1] = (int8_t)mp;
            cm_to[ply + 1] = (uint8_t)move_to(m);
            int q = -quiescence(b, -pc_beta, -pc_beta + 1, ply + 1);
            if (q >= pc_beta) {
                int v = -pvs(b, -pc_beta, -pc_beta + 1, depth - 4, ply + 1, false, m, true, NO_MOVE);
                unmake_move(b, m);
                if (v >= pc_beta) return v;
            } else {
                unmake_move(b, m);
            }
            tried++;
        }
    }

    MoveList moves;
    generate_moves(b, &moves);
    if (moves.count == 0)
        return in_check ? -MATE_SCORE + ply : 0;

    score_moves(b, &moves, tt_move, ply, prev_move);

    int best_score = -INFINITY_SCORE;
    Move best_move = NO_MOVE;
    int moves_searched = 0;
    int legal_moves = 0;
    Move searched_quiets[MAX_MOVES];
    int n_quiets = 0;
    int quiets_tried = 0;
    int original_alpha = alpha;
    Move searched_captures[MAX_MOVES];
    int  n_captures = 0;

    /* Late move pruning: drop hopeless quiet tail in non-PV nodes */
    int lmp_limit = 3 + depth * depth / 2;

    /* Continuation context indices for LMR and history lookups */
    int p1 = (ply >= 1) ? cm_piece[ply]     : -1;
    int t1 = (ply >= 1) ? cm_to[ply]        :  0;
    int p2 = (ply >= 2) ? cm_piece[ply - 1] : -1;
    int t2 = (ply >= 2) ? cm_to[ply - 1]    :  0;
    int p4 = (ply >= 4) ? cm_piece[ply - 3] : -1;
    int t4 = (ply >= 4) ? cm_to[ply - 3]    :  0;

    for (int i = 0; i < moves.count; i++) {
        Move m = pick_next_move(b, &moves, i);
        if (m == excluded) continue;    /* singular verification skips TT move */

        bool capture = is_capture(b, m);
        bool promo = move_is_promo(m);
        bool quiet = !capture && !promo;

        if (!root && !is_pv && quiet && best_score > -MATE_BOUND) {
            /* LMP */
            if (depth <= 6 && quiets_tried >= lmp_limit) continue;
            /* futility: shallow nodes where eval + margin can't reach alpha */
            if (depth <= 5 && !in_check &&
                static_eval + (improving ? 70 : 95) * depth + 100 <= alpha) continue;
            /* SEE pruning of losing quiets */
            if (depth <= 5 && quiets_tried >= 3 && see_move(b, m) < -20 * depth * depth) continue;
        }

        /* Losing capture pruning in non-PV nodes at low depth */
        if (!root && !is_pv && capture && !promo && depth <= 5 &&
            best_score > -MATE_BOUND && moves_searched >= 4 &&
            see_move(b, m) < -50 * depth)
            continue;

        /* Singular extension: search this node at reduced depth with the TT
         * move excluded; if every alternative fails below tt_score - margin,
         * the TT move is the only good move here and deserves an extension. */
        int ext = 0;
        bool singular_ok = (m == tt_move && excluded == NO_MOVE && !root && depth >= 7 &&
                            tt_depth >= depth - 3 &&
                            (tt_bound == BOUND_LOWER || tt_bound == BOUND_EXACT) &&
                            abs(tt_score) < MATE_BOUND);
        if (singular_ok) {
            int s_beta  = tt_score - 2 * depth;
            int s_depth = (depth - 1) / 2;
            int s_score = pvs(b, s_beta - 1, s_beta, s_depth, ply, false, prev_move, false, m);
            if (s_score < s_beta) {
                ext = 1;
                /* Double extension: move is overwhelmingly singular */
                if (!is_pv && s_score < s_beta - 20 && depth >= 10) ext = 2;
            } else if (s_score >= beta) {
                /* Multi-cut: other moves also fail high */
                return s_score;
            } else if (tt_score >= beta) {
                ext = -1;
            }
        }

        int mp = b->piece_on[move_from(m)] % 6;
        nnue_push_move(b, m);
        make_move(b, m);
        if (is_in_check(b, b->side ^ 1)) {   /* mover left its own king in check */
            unmake_move(b, m);
            continue;
        }
        tt_prefetch(b->hash);
        legal_moves++;
        cm_piece[ply + 1] = (int8_t)mp;
        cm_to[ply + 1] = (uint8_t)move_to(m);

        /* gives-check is only consulted by the LMR decision */
        bool gives_check = (quiet && depth >= 3 && !in_check) && is_in_check(b, b->side);

        int next_depth = depth - 1 + ext;
        if (next_depth < 0) next_depth = 0;

        int score;
        uint64_t nodes_before = search_nodes;
        if (moves_searched == 0) {
            score = -pvs(b, -beta, -alpha, next_depth, ply + 1, is_pv, m, true, NO_MOVE);
        } else {
            int r = 0;
            if (quiet && depth >= 3 && !gives_check && !in_check) {
                r = lmr_table[depth < 63 ? depth : 63][moves_searched < 63 ? moves_searched : 63];
                int hf = move_from(m), ht = move_to(m);
                int hpt = b->piece_on[hf] % 6;
                int h = history_table[b->side ^ 1][hf][ht];
                if (hpt == PAWN) h += pawn_history_table[b->side ^ 1][(b->pieces[WHITE][PAWN] ^ b->pieces[BLACK][PAWN]) % 2048][ht];
                if (p1 >= 0) h += cmh_table[CTX_COUNTER][p1][t1][hpt][ht];
                if (p2 >= 0) h += cmh_table[CTX_FOLLOW ][p2][t2][hpt][ht];
                if (p4 >= 0) h += cmh_table[CTX_FOLLOW4][p4][t4][hpt][ht];
                r -= h / 8192;
                if (is_pv) r--;
                else r++; /* Cut-node penalty */
                
                if (ext > 0) r--;
                if (!improving) r++;
                if (r < 0) r = 0;
                if (r > depth - 2) r = depth - 2;
            }

            int rd = next_depth - r;
            if (rd < 0) rd = 0;
            score = -pvs(b, -alpha - 1, -alpha, rd, ply + 1, false, m, true, NO_MOVE);

            if (!abort_search && score > alpha && r > 0)
                score = -pvs(b, -alpha - 1, -alpha, next_depth, ply + 1, false, m, true, NO_MOVE);
            if (!abort_search && score > alpha && score < beta)
                score = -pvs(b, -beta, -alpha, next_depth, ply + 1, is_pv, m, true, NO_MOVE);
        }

        uint64_t m_nodes = search_nodes - nodes_before;
        if (ply == 0 && moves_searched == 0) root_best_nodes = m_nodes;
        else if (ply == 0 && score > best_score) root_best_nodes = m_nodes;

        unmake_move(b, m);
        if (abort_search) return 0;

        moves_searched++;
        if (quiet) { quiets_tried++; if (n_quiets < MAX_MOVES) searched_quiets[n_quiets++] = m; }
        else if (!promo && n_captures < MAX_MOVES) {
            searched_captures[n_captures++] = m;
        }

        if (score > best_score) {
            best_score = score;
            best_move = m;
            if (root) { search_root_best = m; root_best_score = score; }
        }

        if (score > alpha) alpha = score;

        if (alpha >= beta) {
            if (quiet)
                update_histories(b, ply, m, depth, searched_quiets, n_quiets - 1, prev_move);
            else if (!promo) {
                int atkr = b->piece_on[move_from(m)] % 6;
                int victim = (b->occ_all & (1ULL << move_to(m))) ? (b->piece_on[move_to(m)] % 6) : PAWN;
                if (move_is_ep(m)) victim = PAWN;
                int cap_bonus = 32 * depth * depth + 64 * depth - 64;
                if (cap_bonus > 2048) cap_bonus = 2048;
                if (cap_bonus < 0) cap_bonus = 0;
                update_capture_hist(b->side, atkr, move_to(m), victim, cap_bonus);
                for (int ci = 0; ci < n_captures; ci++) {
                    Move cmov = searched_captures[ci];
                    if (cmov == m) continue;
                    int catkr = b->piece_on[move_from(cmov)] % 6;
                    int cvictim = (b->occ_all & (1ULL << move_to(cmov))) ? (b->piece_on[move_to(cmov)] % 6) : PAWN;
                    if (move_is_ep(cmov)) cvictim = PAWN;
                    update_capture_hist(b->side, catkr, move_to(cmov), cvictim, -cap_bonus);
                }
            }
            break;
        }
    }

    if (legal_moves == 0)
        return in_check ? -MATE_SCORE + ply : 0;

    if (excluded == NO_MOVE) {
        tt_store(b->hash, best_score, best_move, depth,
                 best_score >= beta ? BOUND_LOWER :
                 (best_score > original_alpha ? BOUND_EXACT : BOUND_UPPER), ply);
    }
    return best_score;
}

/* ---- Aspiration windows + iterative deepening ---- */
static int aspirate(Board *b, int prev_score, int depth) {
    if (depth < 5)
        return pvs(b, -INFINITY_SCORE, INFINITY_SCORE, depth, 0, true, NO_MOVE, true, NO_MOVE);

    /* Dynamic initial window: narrower for quiet positions, wider for tactical ones */
    int delta = 10 + abs(prev_score) / 64;
    if (delta < 10) delta = 10;
    if (delta > 50) delta = 50;

    int alpha = prev_score - delta;
    int beta  = prev_score + delta;
    int iter = 0;

    while (true) {
        /* Mate scores live outside +-29000; clamps plus the iteration cap
         * guarantee termination even when the score runs into a mate. */
        if (alpha < -29000) alpha = -29000;
        if (beta  >  29000) beta  =  29000;

        int score = pvs(b, alpha, beta, depth, 0, true, NO_MOVE, true, NO_MOVE);
        if (abort_search) return score;

        if (score <= alpha) {
            /* fail low: widen alpha downward, keep beta */
            alpha = score - delta;
            delta += delta / 2;
        } else if (score >= beta) {
            /* fail high: widen beta upward, keep alpha */
            beta = score + delta;
            delta += delta / 2;
        } else {
            return score;
        }

        if (++iter >= 10)
            return pvs(b, -INFINITY_SCORE, INFINITY_SCORE, depth, 0, true, NO_MOVE, true, NO_MOVE);
    }
}

void search_reset_tables(void) {
    clear_search_tables();
    eval_clear_pawn_hash();
}

static void search_one(Board *b) {
    search_root_best = NO_MOVE;
    root_best_score = 0;
    search_nodes = 0;
    seldepth = 0;
    cm_piece[0] = -1;
    syzygy_hits = 0;
    nnue_prepare_search(b);

    clock_gettime(CLOCK_MONOTONIC, &search_start_ts);
    age_history_tables();

    Move prev_best = NO_MOVE;
    int stable_count = 0;
    int last_score = 0;
    int last_iter_time = 0;
    int prev_depth_score = 0;
    int score_trend = 0;

    for (int depth = worker_quiet ? 1+(worker_id%2) : 1; depth <= search_max_depth; depth++) {
        seldepth = 0;
        int iter_start = elapsed_ms();
        int score = aspirate(b, last_score, depth);
        if (abort_search) break;
        int iter_time = elapsed_ms() - iter_start;
        if (iter_time < 1) iter_time = 1;
        last_score = score;

        bool move_changed = (search_root_best != prev_best);
        if (search_root_best == prev_best) stable_count++;
        else { stable_count = 0; prev_best = search_root_best; }

        int score_diff = score - prev_depth_score;
        if (score_diff > 15)       score_trend = (score_trend > 0) ? score_trend + 1 : 1;
        else if (score_diff < -15) score_trend = (score_trend < 0) ? score_trend - 1 : -1;
        else                       score_trend = 0;

        int ms = elapsed_ms();
        uint64_t reported_nodes=total_nodes();
        long long nps = ms > 0 ? (long long)reported_nodes * 1000 / ms : (long long)reported_nodes;

        if (search_verbose && !worker_quiet && score >= MATE_BOUND) {
            int mate_in = (MATE_SCORE - score + 1) / 2;
            printf("info depth %d seldepth %d score mate %d nodes %llu nps %lld time %d hashfull %d tbhits %llu pv",
                   depth, seldepth, mate_in, (unsigned long long)reported_nodes, nps, ms, tt_hashfull(),
                   (unsigned long long)syzygy_hits);
        } else if (search_verbose && !worker_quiet && score <= -MATE_BOUND) {
            int mate_in = -(MATE_SCORE + score + 1) / 2;
            printf("info depth %d seldepth %d score mate %d nodes %llu nps %lld time %d hashfull %d tbhits %llu pv",
                   depth, seldepth, mate_in, (unsigned long long)reported_nodes, nps, ms, tt_hashfull(),
                   (unsigned long long)syzygy_hits);
        } else if (search_verbose && !worker_quiet) {
            printf("info depth %d seldepth %d score cp %d nodes %llu nps %lld time %d hashfull %d tbhits %llu pv",
                   depth, seldepth, score, (unsigned long long)reported_nodes, nps, ms, tt_hashfull(),
                   (unsigned long long)syzygy_hits);
        }

        /* PV from TT walk */
        if (search_verbose && !worker_quiet) {
            Board pvb = *b;
            int pv_len = 0;
            uint64_t seen[64];
            while (pv_len < depth && pv_len < 64) {
                Move pm = tt_probe_move(pvb.hash);
                if (pm == NO_MOVE) break;
                bool rep = false;
                for (int i = 0; i < pv_len; i++) if (seen[i] == pvb.hash) { rep = true; break; }
                if (rep) break;
                seen[pv_len++] = pvb.hash;
                printf(" %s", move_to_uci(&pvb,pm));
                make_move(&pvb, pm);
            }
            if (pv_len == 0 && search_root_best != NO_MOVE)
                printf(" %s", move_to_uci(b,search_root_best));
            printf("\n");
            fflush(stdout);
        }

        /* Predictive time management: estimate the next iteration's cost from
         * the observed branching factor, and stop when it would not finish
         * inside the budget. Bestmove instability widens the budget, stable
         * or clearly decided positions shrink it. */
        int elapsed = elapsed_ms();
        double ratio = last_iter_time > 0 ? (double)iter_time / last_iter_time : 2.4;
        if (ratio < 1.25) ratio = 1.25;
        double predicted = (double)iter_time * ratio;

        double budget = (double)search_soft_ms;
        double bm_frac = reported_nodes > 0 ? (double)root_best_nodes / reported_nodes : 0;
        if (bm_frac > 0.6) budget *= 0.8;
        if (score > 600 || score < -600) budget *= 0.55;
        if (stable_count >= 4 && abs(score - prev_depth_score) <= 12) budget *= 0.5;
        else if (stable_count >= 2 && !move_changed) budget *= 0.85;
        if (move_changed && depth >= 6) budget *= 1.35;
        if (abs(score_trend) >= 2) budget *= 1.20;

        if (!worker_quiet && elapsed + predicted > budget) break;
        last_iter_time = iter_time;
        prev_depth_score = score;
    }

    if (search_root_best == NO_MOVE) {
        MoveList ml;
        generate_moves(b, &ml);
        int mover = b->side;
        for (int i = 0; i < ml.count; i++) {
            make_move(b, ml.moves[i]);
            bool ok = !is_in_check(b, mover);
            unmake_move(b, ml.moves[i]);
            if (ok) { search_root_best = ml.moves[i]; break; }
        }
    }
}

void search_stop(void) { abort_search = true; }

typedef struct { Board board; int id; } SearchWorker;
static void *helper_search(void *arg) {
    SearchWorker *w=arg; worker_id=w->id;worker_quiet=true;
    clear_search_tables();eval_clear_pawn_hash();
    search_one(&w->board);
    atomic_store_explicit(&node_counts[worker_id],search_nodes,memory_order_relaxed);
    return NULL;
}
void search_iterative_deepening(Board *b) {
    tt_age_step();worker_id=0;worker_quiet=false;
    int count=search_threads;
    tt_set_concurrent(count > 1);
    node_counts=calloc((size_t)count,sizeof(*node_counts));
    pthread_t *threads=calloc((size_t)(count>1?count-1:1),sizeof(*threads));
    SearchWorker *workers=calloc((size_t)(count>1?count-1:1),sizeof(*workers));
    if(!node_counts || !threads || !workers) {
        free(node_counts);free(threads);free(workers);node_counts=NULL;
        tt_set_concurrent(false);
        fprintf(stderr,"Search worker allocation failed; using one thread\n");
        search_one(b);return;
    }
    for(int i=0;i<count;i++) atomic_init(&node_counts[i],0);
    active_workers=count;int launched=0;
    for(int i=1;i<count;i++) {
        workers[i-1].board=*b;workers[i-1].id=i;
        if(pthread_create(&threads[i-1],NULL,helper_search,&workers[i-1])) {
            fprintf(stderr,"Could start only %d of %d search threads\n",i,count);break;
        }
        launched++;
    }
    search_one(b);
    uint64_t primary_nodes=search_nodes;
    atomic_store(&abort_search,true);
    atomic_store_explicit(&node_counts[0],primary_nodes,memory_order_relaxed);
    for(int i=0;i<launched;i++) pthread_join(threads[i],NULL);
    tt_set_concurrent(false);
    search_nodes=total_nodes();
    if(search_verbose) {
        int ms=elapsed_ms();
        printf("info string smp threads %d nodes %llu time %d nps %llu\n",launched+1,
               (unsigned long long)search_nodes,ms,
               (unsigned long long)(ms?search_nodes*1000/(unsigned)ms:search_nodes));
        fflush(stdout);
    }
    free(workers);free(threads);free(node_counts);node_counts=NULL;active_workers=0;
}
