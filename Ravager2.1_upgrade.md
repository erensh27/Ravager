# Ravager 2.1 — Upgrade Plan (+100–150 ELO from Ravager 2)

> **IMPORTANT**
> This document is the **master specification** for Ravager 2.1. Every change listed below
> is sourced from gaps found in the Ravager 2 source code. NNUE architecture is **untouched**.
> All improvements target the search, move ordering, pruning, evaluation shell, and TT.

---

## Executive Summary

After a full audit of all source files, the following high-impact gaps were identified:

| Area | Gap | Expected ELO gain |
|------|-----|-------------------|
| Search | Singular extension fires only in non-PV nodes; double/negative extension missing | ~20 ELO |
| Search | LMR formula uses only butterfly history (no CMH in reduction) | ~15 ELO |
| Search | History update does not penalise quiet captures (capture history absent) | ~10 ELO |
| Search | Quiescence search has no TT probe/store | ~12 ELO |
| Search | Aspiration window uses fixed initial delta (18 cp) | ~5 ELO |
| Search | IIR triggers only when no TT hit, not when TT move exists but depth is stale | ~5 ELO |
| Move ordering | Capture scoring ignores SEE for winning captures (over-promotes MVV) | ~8 ELO |
| Move ordering | No capture history (separate from quiet butterfly) | ~8 ELO |
| TT | Two-entry bucket doesn't use age-difference replacement strategy | ~5 ELO |
| Time management | Soft-limit scaling ignores score oscillation between iterations | ~5 ELO |
| Eval (shell) | King danger omits contact checks and queen-contact weight | ~5 ELO |
| Eval (shell) | Rook-on-7th rank bonus absent | ~3 ELO |
| Eval (shell) | Passed pawn "blocked" discount absent | ~4 ELO |
| Misc | `check_time` sampled every 2048 nodes — too coarse at high NPS | ~3 ELO |

**Total realistic gain: ~108–120 ELO** (margins add, some overlap; conservative estimate ≥100 ELO).

---

## 1. Search Improvements

### 1.1 Singular Extensions — Fix + Double Extension + Negative Extension

**File:** `src/search.c` lines 454–465

**Current gap:**
```c
// Singular only fires when !is_pv && !root && depth >= 8
if (m == tt_move && excluded == NO_MOVE && !root && !is_pv && depth >= 8 &&
```
- Singular extensions never fire in PV nodes — modern engines (SF, Berserk, Ethereal) allow them in PV nodes.
- No "double extension" when `s_score` is far below `s_beta` (the TT move is *uniquely* dominant).
- No "negative extension" / early return when `s_score >= s_beta` (multi-cut indication).

**Fix:**
```c
int ext = 0;
bool singular_ok = (m == tt_move && excluded == NO_MOVE && !root && depth >= 7 &&
                    tt_depth >= depth - 3 &&
                    (tt_bound == BOUND_LOWER || tt_bound == BOUND_EXACT) &&
                    abs(tt_score) < MATE_BOUND);
if (singular_ok) {
    int s_beta  = tt_score - 2 * depth;
    int s_score = pvs(b, s_beta - 1, s_beta, (depth - 1) / 2, ply, false,
                      prev_move, false, m);
    if (s_score < s_beta) {
        ext = 1;
        /* Double extension: the TT move is overwhelmingly singular */
        if (!is_pv && s_score < s_beta - 20) ext = 2;
    } else if (s_score >= beta) {
        /* Multi-cut: other moves also fail high — early return */
        return s_score;
    } else if (tt_score >= beta) {
        ext = -1;   /* tt move not singular but node is above beta */
    }
}
```

**Why:** Double extensions add ~10 ELO by spending more time on critical moves. Negative extension/multi-cut saves ~5 ELO by short-circuiting hopeless nodes.

---

### 1.2 LMR — Add CMH to Reduction Formula

**File:** `src/search.c` lines 487–496

**Current gap:**
```c
int h = history_table[b->side ^ 1][move_from(m)][move_to(m)];
r -= h / 8192;
```
Only butterfly history used. Both `CTX_COUNTER` and `CTX_FOLLOW` from `cmh_table` should feed into `r`.

**Fix:**
```c
int f2 = move_from(m), t2 = move_to(m);
int pt2 = b->piece_on[f2] % 6;
int h = history_table[b->side ^ 1][f2][t2];
if (p1 >= 0) h += cmh_table[CTX_COUNTER][p1][t1][pt2][t2];
if (p2 >= 0) h += cmh_table[CTX_FOLLOW ][p2][t2_prev][pt2][t2];
r -= h / 8192;   /* combined 3-ply history drives reduction */
```
Where `p1/t1/p2/t2_prev` come from the cm_piece/cm_to stack already populated in `score_moves`.

**Why:** CMH tracks piece-to-square correlation over 1 and 2 plies — more context = better reduction decisions. Ethereal/Stockfish show +8–12 ELO from combined history in reductions.

---

### 1.3 Capture History Table

**File:** `src/search.c`

**Current gap:** Captures are ordered by pure MVV-LVA + SEE. No history bonuses for previously good captures.

**Fix — add a new table:**
```c
/* [side][attacker_piece][to][captured_piece] */
static int capture_history[2][6][64][6];
```

Update on beta cutoffs (bonus) and failed captures (malus) using same gravity formula:
```c
static void update_capture_history(Board *b, Move best_cap,
                                   Move *searched_caps, int n_caps, int depth) {
    int side = b->side;
    int bonus = 32 * depth;
    // Apply gravity to capture_history[side][attacker][to][victim] for best_cap (+bonus)
    // Apply gravity for all searched_caps[] (-bonus)
}
```

In `score_moves`, add capture history to capture scores:
```c
if (is_capture) {
    int victim = ...;
    int attacker = b->piece_on[move_from(m)] % 6;
    int see_val = see_move(b, m);
    if (see_val >= 0) {
        score = (1 << 22) + see_val * 4 + mvv_victim[victim] * 16
              + capture_history[side][attacker][move_to(m)][victim] / 16;
    } else {
        score = (1 << 19) + see_val;
    }
}
```

**Why:** Capture history distinguishes strategically useful recaptures from tactically forced ones. ~8–10 ELO improvement.

---

### 1.4 Quiescence — TT Probe + Store

**File:** `src/search.c` lines 227–283

**Current gap:** `quiescence()` never touches the TT. Modern engines probe/store at depth=0.

**Fix — add TT probe at top of quiescence:**
```c
static int quiescence(Board *b, int alpha, int beta, int ply) {
    if (abort_search) return 0;
    if (ply >= MAX_PLY - 1) return nnue_eval(b);

    search_nodes++;
    if (ply > seldepth) seldepth = ply;
    if (is_dead_draw(b)) return 0;

    /* TT probe at depth 0 */
    int tt_score = 0; Move tt_move = NO_MOVE;
    int tt_depth = -1, tt_bound = BOUND_UPPER;
    bool tt_hit = tt_probe(b->hash, &tt_score, &tt_move, &tt_depth, &tt_bound, ply);
    if (tt_hit) {
        if (tt_bound == BOUND_EXACT)                      return tt_score;
        if (tt_bound == BOUND_LOWER && tt_score >= beta)  return tt_score;
        if (tt_bound == BOUND_UPPER && tt_score <= alpha) return tt_score;
    }
    int original_alpha = alpha;

    // ... existing logic, passing tt_move to score_moves ...

    /* TT store after search */
    tt_store(b->hash, best_score, best_move, 0,
             best_score >= beta ? BOUND_LOWER :
             (best_score > original_alpha ? BOUND_EXACT : BOUND_UPPER), ply);
    return best_score;
}
```

**Why:** QS TT probes avoid re-evaluating tactical positions reached from many paths. Well-known +10–15 ELO improvement.

---

### 1.5 Aspiration Windows — Dynamic Initial Delta

**File:** `src/search.c` lines 542–575

**Current gap:** Initial delta is always 18 cp regardless of position volatility.

**Fix:**
```c
static int aspirate(Board *b, int prev_score, int depth) {
    if (depth < 5)
        return pvs(b, -INFINITY_SCORE, INFINITY_SCORE, depth, 0, true, NO_MOVE, true, NO_MOVE);

    /* Narrow for quiet positions, wider for high-score/tactical ones */
    int delta = 10 + abs(prev_score) / 64;
    // ... rest of loop unchanged, just starting from dynamic delta ...
}
```

**Why:** Score-adaptive delta prevents over-researching quiet positions and under-researching tactical ones.

---

### 1.6 Internal Iterative Reduction (IIR) — Correct Trigger

**File:** `src/search.c` line 338–339

**Current gap:**
```c
if (!tt_hit && depth >= 4 && !root) depth -= 1;
```
IIR should also fire when we have a TT hit but the stored depth is stale (depth << current).

**Fix:**
```c
bool iir = (!tt_hit || tt_depth < depth - 4) && depth >= 4 && !root && excluded == NO_MOVE;
if (iir) depth -= 1;
```

**Why:** Stale TT entries give unreliable TT moves; reducing depth avoids wasting time on poor guidance. Also extends IIR benefit to PV nodes with stale TT.

---

### 1.7 Null Move — Improved R Scaling

**File:** `src/search.c` lines 364–384

**Current gap:**
```c
int R = 3 + depth / 5;
if (static_eval - beta > 200) R++;
```

**Fix:**
```c
int R = 3 + depth / 4 + (improving ? 0 : 1);
if (static_eval - beta > 300) R++;
/* Don't reduce so aggressively that we skip tactical rechecks */
if (R > depth - 1) R = depth - 1;
```

**Why:** More aggressive R at lower depths gives bigger time savings; the `improving` flag guides when aggression is safe.

---

### 1.8 Futility Pruning — Capture Futility

**File:** `src/search.c` lines 444–452

**Current gap:** Futility only applies to quiet moves. Losing captures at depth ≤ 2 deserve futility too.

**Fix — add before `make_move` in the move loop:**
```c
if (!is_pv && !in_check && capture && !move_is_promo(m) && depth <= 2 &&
    best_score > -MATE_BOUND && moves_searched >= 1) {
    int victim_val = (b->occ_all & (1ULL << move_to(m)))
                     ? mvv_victim[b->piece_on[move_to(m)] % 6] : 0;
    if (static_eval + victim_val + 200 <= alpha) continue;
}
```

**Why:** Avoids spending nodes on clearly losing captures in terminal positions.

---

### 1.9 SEE Pruning of Losing Captures in Non-PV Nodes

**File:** `src/search.c` lines 444–452

**Current gap:** Quiet SEE pruning exists (`see_move(b, m) < -20 * depth * depth`) but there is no equivalent for *losing captures* in non-PV nodes at higher depth.

**Fix:**
```c
/* SEE-prune losing captures in non-PV non-root nodes */
if (!is_pv && !in_check && !root && capture && !move_is_promo(m) &&
    best_score > -MATE_BOUND && depth <= 6 &&
    moves_searched >= 4 && see_move(b, m) < -50 * depth)
    continue;
```

---

### 1.10 `check_time` Sampling Rate

**File:** `src/search.c` lines 51–55

**Current gap:**
```c
if ((search_nodes & 2047) == 0 && elapsed_ms() >= search_hard_ms)
```
At 5–10M NPS (achievable with NNUE enabled and `-march=native`), sampling every 2048 nodes gives ~0.2–0.4ms granularity. Tighten to 1023:

**Fix:**
```c
if ((search_nodes & 1023) == 0 && elapsed_ms() >= search_hard_ms)
    abort_search = true;
```

---

## 2. Move Ordering Improvements

### 2.1 Full SEE-Based Capture Ordering

**File:** `src/search.c` lines 162–169

**Current gap:** SEE only gates captures where `victim < attacker`. Equal captures (PxP) where the pawn is protected are scored incorrectly as "good".

**Fix — rewrite the capture scoring block:**
```c
else if (is_capture(b, m)) {
    int victim   = (b->occ_all & (1ULL << move_to(m))) ? (b->piece_on[move_to(m)] % 6) : PAWN;
    int attacker = b->piece_on[move_from(m)] % 6;
    int see_val  = see_move(b, m);

    if (see_val >= 0) {
        /* Winning/neutral capture: rank by SEE value, then MVV-LVA tiebreak */
        score = (1 << 22) + see_val * 4
              + mvv_victim[victim] * 16 - mvv_attacker[attacker]
              + capture_history[side][attacker][move_to(m)][victim] / 16;
    } else {
        /* Losing capture: rank below killers, above trash quiets */
        score = (1 << 19) + see_val;   /* see_val is negative */
    }
}
```

**Why:** Correctly ordering good vs. bad captures by SEE is +5–8 ELO (Weiss, Ethereal). Combined with capture history, this is the single biggest move-ordering win.

---

### 2.2 TT Move in Quiescence (Prerequisite: §1.4)

After fixing §1.4, the `qtt_move` retrieved from TT must be passed to `score_moves`:
```c
score_moves(b, &caps, qtt_move, ply, NO_MOVE);   /* was: NO_MOVE */
```

This ensures the previous best capture is tried first in quiescence.

---

### 2.3 History Bonus Tuning

**File:** `src/search.c` lines 76–79, 84

**Current gap:**
```c
int bonus = depth * depth;
```
With depth up to 64, max bonus = 4096. The gravity formula:
```c
*h += bonus - *h * abs(bonus) / 16384;
```
gives effective saturation at ±16384, but convergence is slow because raw `depth*depth` bonuses grow too fast for deep nodes and too slow for shallow ones.

**Fix — use the Stockfish-style clamped formula:**
```c
int bonus = 32 * depth * depth + 64 * depth - 64;
if (bonus > 2048) bonus = 2048;   /* cap per-update contribution */
```

**Why:** Faster convergence at shallow depths means the history correctly discards bad moves by move 2–3 of an ID iteration.

---

### 2.4 Killer Reset Between Iterations

**File:** `src/search.c` — `age_history_tables()` lines 124–135

**Current gap:** Killers from the previous iteration at a different depth are stale but still used with full priority.

**Fix — in `age_history_tables`:**
```c
static void age_history_tables(void) {
    // ... existing history halving ...
    /* Shift killers down by 2 plies and clear root killers */
    for (int ply = MAX_PLY - 1; ply >= 2; ply--) {
        killers[ply][0] = killers[ply - 2][0];
        killers[ply][1] = killers[ply - 2][1];
    }
    killers[0][0] = killers[0][1] = NO_MOVE;
    killers[1][0] = killers[1][1] = NO_MOVE;
}
```

---

## 3. Transposition Table Improvements

### 3.1 Improved Two-Entry Replacement Strategy

**File:** `src/tt.c` lines 54–73

**Current gap:** The `depth_preferred` replacement condition:
```c
if (dp->key == key || depth >= dp->depth || dp->age != tt_age)
    *dp = ne;
```
Over-replaces: a high-quality entry with depth 15 is replaced by a depth-15 entry from a *different* node (different key), losing the old data. Also, `always_replace` is written unconditionally even when `depth_preferred` already holds the same key at higher depth.

**Fix:**
```c
void tt_store(...) {
    ...
    TTEntry *dp = &bucket->depth_preferred;
    TTEntry *ar = &bucket->always_replace;

    /* Quality heuristic: penalise old entries and reward deeper entries */
    int dp_quality = (int)dp->depth - 4 * (int8_t)(tt_age - dp->age);
    int ne_quality = (int)depth;

    if (dp->key != key || ne_quality >= dp_quality) {
        *dp = ne;
    }
    /* always_replace: absorb newest regardless */
    *ar = ne;
}
```

**Why:** Avoids prematurely evicting deep entries that are still current-age. The age penalty gives correct priority to fresh entries.

---

### 3.2 TT Entry — Store Static Eval

**File:** `src/tt.c`, `src/tt.h`

**Current gap:** `static_eval` is re-computed via NNUE on every node visit even on TT hits.

**Fix — extend TTEntry (stays at 16 bytes by repurposing `pad`):**
```c
typedef struct {
    uint32_t key;
    int16_t  score;
    int16_t  static_eval;   /* was: two int8_t fields */
    uint16_t move;
    uint8_t  depth;
    uint8_t  bound;
    int8_t   age;
    /* pad removed — now encoded in bound's upper bits or via age */
} TTEntry;  /* 16 bytes */
```

Expose via `tt_probe`:
```c
bool tt_probe(uint64_t hash, int *score, Move *move, int *depth,
              int *bound, int *static_eval, int ply);
```

In `pvs`, use cached static eval on TT hit:
```c
int static_eval;
if (tt_hit && tt_static_eval != -INFINITY_SCORE)
    static_eval = tt_static_eval;
else {
    static_eval = in_check ? -INFINITY_SCORE : nnue_eval(b);
    /* will be stored in tt_store below */
}
```

**Why:** NNUE eval is ~100–200ns per call. At 5M NPS with 50% TT hit rate, this saves 250k+ NNUE calls/sec.

---

### 3.3 TT Prefetch

**File:** `src/tt.h` (new inline), `src/search.c` (call site)

**Current gap:** No cache prefetching. At NPS > 3M, TT cache misses (~50–100 ns) dominate per-node time.

**Fix — add to `tt.h`:**
```c
/* Forward declaration — tt_table and tt_buckets are defined in tt.c */
extern TTBucket *tt_table;
extern uint64_t  tt_buckets;

static inline void tt_prefetch(uint64_t hash) {
    if (!tt_table) return;
    uint64_t idx = (uint64_t)((__uint128_t)hash * (__uint128_t)tt_buckets >> 64);
    __builtin_prefetch((const char*)&tt_table[idx], 0, 1);
}
```

**Call site in `pvs`, immediately after `make_move`:**
```c
make_move(b, m);
if (is_in_check(b, b->side ^ 1)) { unmake_move(b, m); continue; }
tt_prefetch(b->hash);   /* prefetch child's TT entry while we do other work */
int score = -pvs(b, ...);
```

**Why:** `__builtin_prefetch` is a zero-cycle hint that initiates a cache-line fetch 50–100 ns early. Standard +3–7% NPS improvement in all modern engines.

---

## 4. Evaluation Shell Improvements

> **NOTE:** NNUE is untouched. These HCE shell improvements affect pruning accuracy
> (RFP margins, null move conditions) and provide a safety net when NNUE is disabled.

### 4.1 Rook on 7th Rank

**File:** `src/evaluate.c` — inside the Rooks loop (~line 294)

**Current gap:** No bonus for a rook on the 7th rank trapping the enemy king on the 8th.

**Fix:**
```c
/* Rook on 7th: valuable when enemy king is on back rank or enemy pawns are on 7th */
{
    int r7 = (c == WHITE) ? 6 : 1;
    int r8 = (c == WHITE) ? 7 : 0;
    Bitboard rank7_opp = (c == WHITE) ? RANK_7 : RANK_2;
    if (rank_of(sq) == r7) {
        bool king_on_back = (rank_of(lsb(b->pieces[opp][KING])) == r8);
        bool pawns_on_7th = (b->pieces[opp][PAWN] & rank7_opp) != 0;
        if (king_on_back || pawns_on_7th) {
            total.mg += sign * ROOK_ON_7TH[MG];
            total.eg += sign * ROOK_ON_7TH[EG];
        }
    }
}
```

Add to `params.h`: `extern int32_t ROOK_ON_7TH[2];`
Add to `params.c`: `int32_t ROOK_ON_7TH[2] = { 16, 32 };`

---

### 4.2 Passed Pawn — Blocked Discount

**File:** `src/evaluate.c` — passed pawn loop (~line 414)

**Current gap:** A passed pawn blocked by an enemy piece on its stop square still gets full `PASSED_BASE` bonus.

**Fix:**
```c
int stop_sq = (c == WHITE) ? sq + 8 : sq - 8;
bool blocked = (stop_sq >= 0 && stop_sq < 64 && (b->occ_all & (1ULL << stop_sq)) != 0);
if (blocked) {
    /* Apply fraction of the base bonus — the passer cannot advance freely */
    total.mg += sign * PASSED_BASE[MG][rel] / 3;
    total.eg += sign * PASSED_BASE[EG][rel] / 3;
} else {
    total.mg += sign * PASSED_BASE[MG][rel];
    total.eg += sign * PASSED_BASE[EG][rel];
}
```

**Why:** Blocked passers are fundamentally less valuable — classic endgame theory. Distinguishing them prevents the engine from over-valuing blocked passed pawns.

---

### 4.3 King Safety — Contact Checks

**File:** `src/evaluate.c` — king safety loop (~line 240)

**Current gap:** Attack units only increment for pieces attacking the king *zone*, not pieces that directly touch the king square.

**Fix — inside the per-piece attack-unit counting:**
```c
if (att & opp_kz) {
    attack_units[opp] += king_attack_weight[KNIGHT] / 10;
    attacker_count[opp]++;
}
/* Contact check bonus: piece directly attacks the king square */
if (att & (1ULL << opp_ksq)) {
    attack_units[opp] += king_attack_weight[KNIGHT] / 5;  /* extra weight */
}
```

Also increase the queen king-attack weight:
```c
static const int king_attack_weight[6] = { 0, 20, 20, 40, 100, 0 };
/*                                                         was 80 ^^^^ */
```

**Why:** Contact checks are extremely dangerous and should register much higher danger than distant zone attacks.

---

### 4.4 Weak Squares — Minor Piece Targets

**File:** `src/evaluate.c` — after threats section (~line 360)

**Current gap:** No evaluation of weak squares in the enemy camp that our minor pieces can occupy.

**Fix:**
```c
/* Weak squares: squares in enemy half not defended by enemy pawns */
{
    Bitboard enemy_half = (c == WHITE)
        ? (RANK_5 | RANK_6 | RANK_7 | RANK_8)
        : (RANK_4 | RANK_3 | RANK_2 | RANK_1);
    Bitboard weak_sq = enemy_half & ~enemy_pawn_att;
    Bitboard own_minors = b->pieces[c][KNIGHT] | b->pieces[c][BISHOP];
    int controlled = 0;
    Bitboard tmp = own_minors;
    while (tmp) {
        int sq2 = lsb_pop(&tmp);
        Bitboard attacks = (b->piece_on[sq2] % 6 == KNIGHT)
                         ? knight_attack_table[sq2]
                         : bishop_attacks(sq2, occ);
        controlled += popcount(attacks & weak_sq & ~b->occupancy[c]);
    }
    total.mg += sign * controlled * WEAK_SQ_BONUS[MG];
    total.eg += sign * controlled * WEAK_SQ_BONUS[EG];
}
```

Add to `params.h`: `extern int32_t WEAK_SQ_BONUS[2];`
Add to `params.c`: `int32_t WEAK_SQ_BONUS[2] = { 3, 5 };`

---

## 5. Time Management Improvements

### 5.1 Score Oscillation Detection

**File:** `src/search.c` lines 655–672

**Current gap:** Time budget scaling ignores score oscillation (score flipping direction between iterations).

**Fix — track score trend in `search_iterative_deepening`:**
```c
int score_trend = 0;
// ... inside the depth loop, after getting `score`:
int score_diff = score - prev_depth_score;
if (score_diff > 15)       score_trend = (score_trend > 0) ? score_trend + 1 : 1;
else if (score_diff < -15) score_trend = (score_trend < 0) ? score_trend - 1 : -1;
else                       score_trend = 0;

double budget = (double)search_soft_ms;
if (abs(score_trend) >= 2) budget *= 1.20;  /* unstable eval: search longer */
// ... rest of existing budget adjustments ...
```

---

### 5.2 Improved Increment Utilization

**File:** `src/uci.c` lines 231–241

**Current gap:** Fixed `moves_left = 30` ignores game phase. Hard limit is `5 * soft`, which can overshoot in bullet.

**Fix:**
```c
int moves_left = movestogo > 0 ? movestogo : 25;
if (!movestogo) {
    /* Phase-aware estimate: more time in complex middlegame */
    int phase = main_board.game_phase;  /* 0=endgame, 24=opening */
    moves_left = 18 + phase / 4;        /* 18–24 depending on phase */
    if (game_hist_len >= 80) moves_left -= 4;  /* late-game speed-up */
}
int base = my_time / moves_left;
int soft = base + my_inc * 3 / 4 - move_overhead_ms;
int hard = soft * 4 + my_inc / 2;
/* Hard cap: never spend more than 75% of clock on one move */
if (hard > my_time * 3 / 4) hard = my_time * 3 / 4;
if (hard < soft) hard = soft;
search_soft_ms = soft > 1 ? soft : 1;
search_hard_ms = hard;
```

---

## 6. NPS / Performance Optimizations

### 6.1 SEE — Fast Path for Uncontested Captures

**File:** `src/see.c` lines 61–67

**Current gap:** Full swap-list algorithm even when there is no recapture possible.

**Fix — add pre-check in `see_move`:**
```c
int see_move(Board *b, Move m) {
    int from = move_from(m), to = move_to(m);
    int target   = (b->occ_all & (1ULL << to)) ? (b->piece_on[to] % 6) : NO_PIECE;
    int attacker = b->piece_on[from] % 6;
    if (move_is_ep(m)) target = PAWN;

    /* Fast path: no recapture possible after removing the attacker */
    Bitboard occ_after = b->occ_all ^ (1ULL << from);
    Bitboard defenders = all_attackers_to(b, to, occ_after) & b->occupancy[b->side ^ 1];
    if (!defenders)
        return (target != NO_PIECE) ? see_piece_val[target] : 0;

    return see(b, to, target, from, attacker);
}
```

**Why:** Many captures in quiet positions have no recapture — this fast path avoids 10–15 bitboard ops per such capture, measurably improving NPS.

---

### 6.2 Staged Move Generation

**Current gap:** `generate_moves` generates *all* pseudo-legal moves at once. In cut nodes where the TT move or first killer causes a beta cutoff, all the remaining moves were generated and scored for nothing.

**Fix — add a staged move picker struct:**
```c
typedef struct {
    MoveList ml;
    int stage;         /* 0=tt, 1=gen_caps+score, 2=caps, 3=killers/cm, 4=quiets */
    int idx;
    Move tt_move;
    Move killers[2];
    Move counter_move;
} MovePicker;

static void mp_init(MovePicker *mp, Move tt_move, Move k0, Move k1, Move cm);
static Move mp_next(MovePicker *mp, Board *b, int ply);
```

Replace the single `generate_moves + score_moves + pick_next_move` loop in `pvs` with `mp_next` calls. Generate captures first, then quiets only if needed.

**Expected benefit:** ~10–15% NPS improvement in positions with early cutoffs (which are the majority in alpha-beta), and significantly fewer `score_moves` calls.

---

### 6.3 `__attribute__((hot))` on Critical Functions

**Fix — in relevant headers or `.c` files:**
```c
__attribute__((hot)) static int pvs(Board *b, int alpha, int beta, int depth,
                                    int ply, bool is_pv, Move prev_move,
                                    bool null_ok, Move excluded);

__attribute__((hot)) static int quiescence(Board *b, int alpha, int beta, int ply);
```

This hints to GCC/Clang to optimize these functions more aggressively and place them in hot code sections for better instruction cache utilization.

---

## 7. Correctness Fixes

### 7.1 Repetition Detection — Half-Move Clock Boundary

**File:** `src/search.c` lines 205–219

**Current gap:**
```c
if (start - i > b->half_move_clock + 2) break;
```
Uses the *current* half-move clock. But captures during the search reset this clock, potentially causing the loop to miss repetitions in the game history if a capture happened mid-search.

**Fix:** Store root's half-move clock at search start:
```c
static int root_half_move_clock = 0;
// In search_iterative_deepening, before the depth loop:
root_half_move_clock = b->half_move_clock + b->ply;

// In is_repetition:
int max_lookback = root_half_move_clock + b->ply;
for (int i = start - 2; i >= 0; i -= 2) {
    if (start - i > max_lookback) break;
    ...
}
```

---

### 7.2 Draw Score — Contempt

**Current gap:** Draws always return `0`. In winning positions, the engine should be reluctant to accept draws.

**Fix — add a search-global:**
```c
static int contempt_mg = 8;   /* tunable via UCI option */

// In pvs, replace:
if (is_repetition(b)) return 0;
// With:
if (is_repetition(b)) {
    /* Return a small penalty for the side that should be winning */
    return (b->side == root_side) ? -contempt_mg : contempt_mg;
}
```

Expose via UCI: `option name Contempt type spin default 8 min -50 max 50`

---

## 8. Implementation Roadmap

### Phase 1 — High-Impact Search (implement first)
1. **§1.4** — QS TT probe/store
2. **§1.2** — LMR with CMH
3. **§1.3 + §2.1** — Capture history + SEE ordering
4. **§3.3** — TT prefetch

### Phase 2 — Extensions + Pruning
5. **§1.1** — Singular double/negative extension
6. **§1.6** — IIR trigger fix
7. **§1.7** — Null move R tuning
8. **§1.8 + §1.9** — Capture futility + losing-capture pruning

### Phase 3 — TT + Evaluation Shell
9. **§3.2** — TT stores static eval
10. **§3.1** — Improved replacement strategy
11. **§4.1** — Rook on 7th
12. **§4.2** — Passed pawn blocked discount
13. **§4.3** — King safety contact checks
14. **§4.4** — Weak squares

### Phase 4 — NPS + Time Management
15. **§6.2** — Staged move generation (largest refactor)
16. **§5.1 + §5.2** — Time management improvements
17. **§2.3 + §2.4** — History bonus tuning + killer aging
18. **§1.5** — Aspiration dynamic delta
19. **§7.1 + §7.2** — Correctness: repetition + contempt

---

## 9. Files to Change

| File | Changes |
|------|---------|
| `src/search.c` | §1.1–1.10, §2.1–2.4, §5.1, §7.1–7.2, capture history table |
| `src/tt.c` | §3.1–3.2 (improved replacement, static eval storage) |
| `src/tt.h` | §3.3 (prefetch inline), extended `tt_probe` signature |
| `src/evaluate.c` | §4.1–4.4 (rook 7th, passed blocked, contact, weak sq) |
| `src/params.h` | New params: `ROOK_ON_7TH`, `WEAK_SQ_BONUS` |
| `src/params.c` | Initial values for new params |
| `src/see.c` | §6.1 (uncontested capture fast path) |
| `src/uci.c` | §5.2 (time management), §7.2 contempt UCI option |
| `src/ravager.h` | Version bump to `"2.1"` |

---

## 10. Expected ELO Breakdown

| Change | Est. ELO |
|--------|----------|
| QS TT probe/store (§1.4) | +12 |
| LMR + CMH integration (§1.2) | +12 |
| Capture history + SEE ordering (§1.3, §2.1) | +15 |
| Singular double/negative ext (§1.1) | +12 |
| TT prefetch (§3.3) | +4 (NPS) |
| IIR trigger fix (§1.6) | +5 |
| TT stores static eval (§3.2) | +4 |
| Null move R tuning (§1.7) | +4 |
| Rook on 7th (§4.1) | +4 |
| Passed pawn blocked (§4.2) | +4 |
| History bonus formula (§2.3) | +4 |
| Time management oscillation (§5.1) | +5 |
| Aspiration dynamic delta (§1.5) | +3 |
| Capture futility (§1.8) | +3 |
| King safety contact (§4.3) | +4 |
| Contempt (§7.2) | +3 |
| Staged movegen (§6.2, partial) | +5 (NPS) |
| **Subtotal (with ~25% overlap)** | **~108–118 ELO** |

> **TIP:** Implement in Phase order. Run a 100-game SPRT test (H0: 0 ELO, H1: +10 ELO,
> α=0.05, β=0.05) after each phase. Use `./ravager bench` as a quick NPS sanity check
> between every code change.
