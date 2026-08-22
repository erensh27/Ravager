/* tuner.c — texel tuner (Texel 1.07 method, local-search variant).
 *
 * Minimises E = 1/N * sum (result - sigmoid(eval/K))^2 over the evaluation
 * parameters in params.c. PST entries use a closed-form delta (the tapered
 * score is linear in them); every other parameter is optimised by finite
 * differences. Rewrites params.c with the tuned values.
 *
 * Build: make tuner
 * Run:   ./tuner training.txt src/params.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "bitboard.h"
#include "params.h"
#include "ravager.h"

void eval_clear_pawn_hash(void);

#define MAX_POS 120000

/* Positions are stored as compact snapshots; each evaluation rebuilds one
 * reusable board (Board carries a large undo stack, so keeping tens of
 * thousands alive is not viable). */
typedef struct {
    uint8_t piece_on[64];
    uint8_t color_on[64];
    uint8_t side;
} Snap;

static Snap snaps[MAX_POS];
static double results[MAX_POS];
static double evals[MAX_POS];      /* white-POV eval in cp */
static int    ph_of[MAX_POS];      /* game phase 0..24 */
static int    n_pos = 0;
static Board work;                 /* scratch board */

static double K = 120.0;           /* sigmoid scaling in centipawns */

static inline double sigmoid(double e) {
    return 1.0 / (1.0 + exp(-e / K));
}

static void load_pos(int i) {
    memset(&work, 0, sizeof(work));
    Snap *s = &snaps[i];
    for (int sq = 0; sq < 64; sq++) {
        work.piece_on[sq] = EMPTY_SQUARE;
        work.color_on[sq] = 0;
    }
    work.ep_square = NO_SQUARE;
    work.full_move_number = 1;
    for (int sq = 0; sq < 64; sq++) {
        if (s->piece_on[sq] == EMPTY_SQUARE) continue;
        int c = s->color_on[sq], p = s->piece_on[sq];
        Bitboard bit = 1ULL << sq;
        work.pieces[c][p] |= bit;
        work.occupancy[c] |= bit;
        work.occ_all |= bit;
        work.piece_on[sq] = (uint8_t)p;
        work.color_on[sq] = (uint8_t)c;
    }
    work.side = s->side;
    board_refresh_psqt(&work);
}

static double white_eval(int i) {
    load_pos(i);
    int e = evaluate(&work);
    return (work.side == WHITE) ? e : -e;
}

static double total_error(void) {
    double sum = 0;
    for (int i = 0; i < n_pos; i++)
        sum += pow(results[i] - sigmoid(evals[i]), 2);
    return sum / n_pos;
}

/* ---- parameter groups (finite differences) ---- */
typedef struct { int32_t *p; int n; const char *name; int lo, hi; } PGroup;

static PGroup groups[] = {
    { (int32_t*)KNIGHT_MOB,        18, "KNIGHT_MOB",      -120, 120 },
    { (int32_t*)BISHOP_MOB,        28, "BISHOP_MOB",      -120, 120 },
    { (int32_t*)ROOK_MOB,          30, "ROOK_MOB",        -120, 120 },
    { (int32_t*)QUEEN_MOB,         56, "QUEEN_MOB",       -120, 120 },
    { (int32_t*)DOUBLED,            2, "DOUBLED",         -100,  40 },
    { (int32_t*)ISOLATED,           2, "ISOLATED",        -100,  40 },
    { (int32_t*)BACKWARD,           2, "BACKWARD",        -100,  40 },
    { (int32_t*)CONNECTED,         16, "CONNECTED",         -20, 120 },
    { (int32_t*)PASSED_BASE,       16, "PASSED_BASE",        -20, 400 },
    { (int32_t*)PASSED_FREE,        2, "PASSED_FREE",        -20,  80 },
    { (int32_t*)PASSED_DEFENDED,    2, "PASSED_DEFENDED",    -20,  80 },
    { (int32_t*)PASSER_KD,         16, "PASSER_KD",           -20, 120 },
    { (int32_t*)BISHOP_PAIR,        2, "BISHOP_PAIR",         -20, 160 },
    { (int32_t*)ROOK_OPEN,          2, "ROOK_OPEN",           -20, 100 },
    { (int32_t*)ROOK_SEMI,          2, "ROOK_SEMI",           -20, 100 },
    { (int32_t*)ROOK_DOUBLED,       2, "ROOK_DOUBLED",        -20, 100 },
    { (int32_t*)ROOK_BEHIND,        2, "ROOK_BEHIND",         -20, 100 },
    { (int32_t*)KNIGHT_OUTPOST,     2, "KNIGHT_OUTPOST",      -20, 100 },
    { (int32_t*)BISHOP_OUTPOST,     2, "BISHOP_OUTPOST",      -20, 100 },
    { (int32_t*)QUEEN_EARLY,        2, "QUEEN_EARLY",       -100,  20 },
    { (int32_t*)BAD_BISHOP,         2, "BAD_BISHOP",        -100,  20 },
    { (int32_t*)SHIELD_MISSING,     2, "SHIELD_MISSING",    -100,   0 },
    { (int32_t*)STORM,             10, "STORM",             -100,  80 },
    { (int32_t*)THREAT_PAWN_MINOR,  2, "THREAT_PAWN_MINOR",-100,  40 },
    { (int32_t*)THREAT_PAWN_ROOKQ,  2, "THREAT_PAWN_ROOKQ",-100,  40 },
    { (int32_t*)THREAT_Q_BY_MINOR,  2, "THREAT_Q_BY_MINOR", -40, 120 },
    { (int32_t*)THREAT_Q_BY_ROOK,   2, "THREAT_Q_BY_ROOK",  -40, 120 },
    { (int32_t*)SPACE,              2, "SPACE",             -10,  20 },
    { (int32_t*)TEMPO,              2, "TEMPO",               0,  50 },
};

/* per (piece,square) list of position indices for closed-form PST deltas */
static int *pst_list[6][64];
static int  pst_cnt[6][64];

static void build_pst_lists(void) {
    for (int p = 0; p < 6; p++)
        for (int sq = 0; sq < 64; sq++) {
            pst_cnt[p][sq] = 0;
        }
    for (int i = 0; i < n_pos; i++) {
        for (int sq = 0; sq < 64; sq++) {
            if (snaps[i].piece_on[sq] == EMPTY_SQUARE) continue;
            pst_cnt[snaps[i].piece_on[sq] % 6][sq]++;
        }
    }
    for (int p = 0; p < 6; p++)
        for (int sq = 0; sq < 64; sq++) {
            if (pst_cnt[p][sq] == 0) { pst_list[p][sq] = NULL; continue; }
            pst_list[p][sq] = malloc(pst_cnt[p][sq] * sizeof(int));
            pst_cnt[p][sq] = 0;   /* reused as fill cursor */
        }
    for (int i = 0; i < n_pos; i++)
        for (int sq = 0; sq < 64; sq++) {
            if (snaps[i].piece_on[sq] == EMPTY_SQUARE) continue;
            int p = snaps[i].piece_on[sq] % 6;
            pst_list[p][sq][pst_cnt[p][sq]++] = i;
        }
}

/* closed-form sweep over one PST entry: returns improved error or -1 */
static double pst_try(int p, int ph, int sq, int delta, double E) {
    int *lst = pst_list[p][sq];
    int n = pst_cnt[p][sq];
    if (n == 0) return -1;
    double gain = 0;
    for (int k = 0; k < n; k++) {
        int i = lst[k];
        int w = (snaps[i].color_on[sq] == WHITE) ? 1 : -1;
        double de = (double)delta * w * ((ph == MG) ? ph_of[i] : 24 - ph_of[i]) / 24.0;
        double e2 = evals[i] + de;
        gain += pow(results[i] - sigmoid(e2), 2) - pow(results[i] - sigmoid(evals[i]), 2);
    }
    return E + gain / n_pos;
}

static void pst_apply(int p, int ph, int sq, int delta) {
    PST[p][ph][sq] += delta;
    int *lst = pst_list[p][sq];
    for (int k = 0; k < pst_cnt[p][sq]; k++) {
        int i = lst[k];
        int w = (snaps[i].color_on[sq] == WHITE) ? 1 : -1;
        evals[i] += (double)delta * w * ((ph == MG) ? ph_of[i] : 24 - ph_of[i]) / 24.0;
    }
}

static double fd_error(void) {
    eval_clear_pawn_hash();
    for (int i = 0; i < n_pos; i++)
        evals[i] = white_eval(i);
    return total_error();
}

static double group_sweep(double E, int delta) {
    for (unsigned g = 0; g < sizeof(groups)/sizeof(groups[0]); g++) {
        for (int k = 0; k < groups[g].n; k++) {
            int32_t *par = &groups[g].p[k];
            int orig = *par;
            int best_d = 0;
            double best_E = E;
            for (int d = -delta; d <= delta; d += 2*delta) {
                int v = orig + d;
                if (v < groups[g].lo || v > groups[g].hi) continue;
                *par = v;
                double E2 = fd_error();
                if (E2 < best_E - 1e-12) { best_E = E2; best_d = d; }
            }
            *par = orig + best_d;
        }
    }
    /* resync evals to the final parameter state */
    eval_clear_pawn_hash();
    for (int i = 0; i < n_pos; i++) evals[i] = white_eval(i);
    return total_error();
}

static double pst_sweep(double E, int delta) {
    int since_resync = 0;
    for (int p = 0; p < 6; p++)
        for (int sq = 0; sq < 64; sq++) {
            if (p == PAWN && (rank_of(sq) == 0 || rank_of(sq) == 7)) continue;  /* unused */
            for (int ph = 0; ph < 2; ph++) {
                int orig = PST[p][ph][sq];
                int best_d = 0;
                double best_E = E;
                for (int d = -delta; d <= delta; d += 2*delta) {
                    double E2 = pst_try(p, ph, sq, d, E);
                    if (E2 >= 0 && E2 < best_E - 1e-12) { best_E = E2; best_d = d; }
                }
                if (best_d) {
                    pst_apply(p, ph, sq, best_d);
                    E = best_E;
                    if (++since_resync >= 32) {
                        /* closed-form evals drift from the integer eval; resync */
                        refresh_psqt_tables();
                        E = fd_error();
                        since_resync = 0;
                    }
                }
            }
        }
    refresh_psqt_tables();
    return fd_error();
}

/* ---- params.c regeneration ---- */
static FILE *out;
static void dump_pst(void) {
    fprintf(out, "int32_t PST[6][2][64] = {\n");
    for (int p = 0; p < 6; p++) {
        fprintf(out, "    { /* %d */\n", p);
        for (int ph = 0; ph < 2; ph++) {
            fprintf(out, "        { /* %s */\n", ph ? "EG" : "MG");
            for (int r = 0; r < 8; r++) {
                fprintf(out, "            ");
                for (int f = 0; f < 8; f++)
                    fprintf(out, "%6d,", PST[p][ph][r*8+f]);
                fprintf(out, "\n");
            }
            fprintf(out, "        },\n");
        }
        fprintf(out, "    },\n");
    }
    fprintf(out, "};\n\n");
}
static void dump_arr2(const char *name, int32_t *a, int rows, int cols) {
    fprintf(out, "int32_t %s[%d][%d] = {", name, rows, cols);
    for (int r = 0; r < rows; r++) {
        fprintf(out, "%s    ", r ? "" : "\n");
        for (int c = 0; c < cols; c++)
            fprintf(out, "%5d,", a[r*cols+c]);
        fprintf(out, "\n");
    }
    fprintf(out, "};\n\n");
}

static void dump_params(const char *path) {
    out = fopen(path, "w");
    if (!out) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    fprintf(out, "/* params.c — evaluation parameter values.\n");
    fprintf(out, " *\n");
    fprintf(out, " * TEXEL-TUNED by ravager-tuner. This file is machine-generated territory.\n");
    fprintf(out, " * Values are centipawns. */\n\n");
    fprintf(out, "#include \"params.h\"\n\n");
    dump_pst();
    fprintf(out, "int32_t PHASE_VALUES[6] = { 0, 1, 1, 2, 4, 0 };\n");
    fprintf(out, "int32_t MATERIAL_MG[6] = {  82, 337, 365, 477, 1025, 0 };\n");
    fprintf(out, "int32_t MATERIAL_EG[6] = {  94, 281, 297, 512,  936, 0 };\n\n");
    dump_arr2("KNIGHT_MOB", (int32_t*)KNIGHT_MOB, 2, 9);
    dump_arr2("BISHOP_MOB", (int32_t*)BISHOP_MOB, 2, 14);
    dump_arr2("ROOK_MOB", (int32_t*)ROOK_MOB, 2, 15);
    dump_arr2("QUEEN_MOB", (int32_t*)QUEEN_MOB, 2, 28);
    dump_arr2("DOUBLED", (int32_t*)DOUBLED, 1, 2);
    dump_arr2("ISOLATED", (int32_t*)ISOLATED, 1, 2);
    dump_arr2("BACKWARD", (int32_t*)BACKWARD, 1, 2);
    dump_arr2("CONNECTED", (int32_t*)CONNECTED, 2, 8);
    dump_arr2("PASSED_BASE", (int32_t*)PASSED_BASE, 2, 8);
    dump_arr2("PASSED_FREE", (int32_t*)PASSED_FREE, 1, 2);
    dump_arr2("PASSED_DEFENDED", (int32_t*)PASSED_DEFENDED, 1, 2);
    dump_arr2("PASSER_KD", (int32_t*)PASSER_KD, 2, 8);
    dump_arr2("BISHOP_PAIR", (int32_t*)BISHOP_PAIR, 1, 2);
    dump_arr2("ROOK_OPEN", (int32_t*)ROOK_OPEN, 1, 2);
    dump_arr2("ROOK_SEMI", (int32_t*)ROOK_SEMI, 1, 2);
    dump_arr2("ROOK_DOUBLED", (int32_t*)ROOK_DOUBLED, 1, 2);
    dump_arr2("ROOK_BEHIND", (int32_t*)ROOK_BEHIND, 1, 2);
    dump_arr2("KNIGHT_OUTPOST", (int32_t*)KNIGHT_OUTPOST, 1, 2);
    dump_arr2("BISHOP_OUTPOST", (int32_t*)BISHOP_OUTPOST, 1, 2);
    dump_arr2("QUEEN_EARLY", (int32_t*)QUEEN_EARLY, 1, 2);
    dump_arr2("BAD_BISHOP", (int32_t*)BAD_BISHOP, 1, 2);
    dump_arr2("SHIELD_MISSING", (int32_t*)SHIELD_MISSING, 1, 2);
    dump_arr2("STORM", (int32_t*)STORM, 2, 5);
    dump_arr2("THREAT_PAWN_MINOR", (int32_t*)THREAT_PAWN_MINOR, 1, 2);
    dump_arr2("THREAT_PAWN_ROOKQ", (int32_t*)THREAT_PAWN_ROOKQ, 1, 2);
    dump_arr2("THREAT_Q_BY_MINOR", (int32_t*)THREAT_Q_BY_MINOR, 1, 2);
    dump_arr2("THREAT_Q_BY_ROOK", (int32_t*)THREAT_Q_BY_ROOK, 1, 2);
    dump_arr2("SPACE", (int32_t*)SPACE, 1, 2);
    dump_arr2("TEMPO", (int32_t*)TEMPO, 1, 2);
    fclose(out);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s training.txt out_params.c [rounds]\n", argv[0]);
        return 1;
    }
    const char *datafile = argv[1];
    const char *outfile  = argv[2];
    int rounds = argc > 3 ? atoi(argv[3]) : 4;

    board_init_all();

    FILE *f = fopen(datafile, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", datafile); return 1; }
    char line[256];
    int skipped = 0;
    while (fgets(line, sizeof(line), f) && n_pos < MAX_POS) {
        char *sp = strrchr(line, ' ');
        if (!sp) continue;
        double r = atof(sp + 1);
        *sp = 0;
        if (!parse_fen(&work, line)) continue;
        Snap *sn = &snaps[n_pos];
        for (int sq = 0; sq < 64; sq++) {
            if (work.piece_on[sq] == EMPTY_SQUARE) { sn->piece_on[sq] = EMPTY_SQUARE; sn->color_on[sq] = 0; }
            else { sn->piece_on[sq] = work.piece_on[sq]; sn->color_on[sq] = work.color_on[sq]; }
        }
        sn->side = (uint8_t)work.side;
        double e = white_eval(n_pos);
        if (e > 2000 || e < -2000) { skipped++; continue; }
        results[n_pos] = r;
        evals[n_pos] = e;
        ph_of[n_pos] = work.game_phase > 24 ? 24 : work.game_phase;
        n_pos++;
    }
    fclose(f);
    fprintf(stderr, "loaded %d positions (%d skipped as hopeless)\n", n_pos, skipped);
    if (n_pos < 100) { fprintf(stderr, "not enough data\n"); return 1; }

    /* deterministic shuffle + 10% holdout for honest validation */
    uint64_t rngs = 88172645463325252ULL;
    for (int i = n_pos - 1; i > 0; i--) {
        rngs ^= rngs << 13; rngs ^= rngs >> 7; rngs ^= rngs << 17;
        int j = (int)(rngs % (uint64_t)(i + 1));
        Snap ts = snaps[i]; snaps[i] = snaps[j]; snaps[j] = ts;
        double td = results[i]; results[i] = results[j]; results[j] = td;
        double te = evals[i]; evals[i] = evals[j]; evals[j] = te;
        int tp = ph_of[i]; ph_of[i] = ph_of[j]; ph_of[j] = tp;
    }
    int n_train = n_pos - n_pos / 10;
    fprintf(stderr, "train %d / holdout %d\n", n_train, n_pos - n_train);

    double E = 1e9;
    double startE = 0;
    K = 120;
    startE = total_error();
    fprintf(stderr, "start error (K=120) = %.6f\n", startE);

    n_hold = n_pos - n_train;
    n_pos = n_train;                    /* tune on the training split only */
    build_pst_lists();
    double (*hold_err)() = NULL; (void)hold_err;

    for (int round = 1; round <= rounds; round++) {
        /* reselect K each round over a wide grid */
        double bestK = K, bestEk = 1e9;
        for (double k = 80; k <= 200.01; k += 20) {
            K = k;
            double Ek = total_error();
            if (Ek < bestEk) { bestEk = Ek; bestK = k; }
        }
        K = bestK;
        E = total_error();
        fprintf(stderr, "round %d: K=%.0f  error = %.6f\n", round, K, E);
        int pst_delta = round <= 2 ? 6 : (round == 3 ? 3 : 1);
        int grp_delta = round <= 2 ? 4 : 2;
        E = pst_sweep(E, pst_delta);
        fprintf(stderr, "round %d: pst sweep (d=%d) -> %.6f\n", round, pst_delta, E);
        refresh_psqt_tables();   /* load_pos refreshes accumulators per eval */
        E = fd_error();
        E = group_sweep(E, grp_delta);
        fprintf(stderr, "round %d: group sweep (d=%d) -> %.6f\n", round, grp_delta, E);
    }

    /* holdout error with the final parameters */
    int tune_n = n_pos;
    n_pos = tune_n + n_hold;
    eval_clear_pawn_hash();
    double hold = 0;
    for (int i = tune_n; i < n_pos; i++) {
        double e = white_eval(i);
        hold += pow(results[i] - sigmoid(e), 2);
    }
    hold /= n_hold;
    n_pos = tune_n;
    eval_clear_pawn_hash();
    for (int i = 0; i < n_pos; i++) evals[i] = white_eval(i);
    fprintf(stderr, "final train error = %.6f (from %.6f)  HOLDOUT error = %.6f\n",
            total_error(), startE, hold);
    dump_params(outfile);
    fprintf(stderr, "wrote %s\n", outfile);
    return 0;
}
