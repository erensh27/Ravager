/* nnue.c — NNUE loading, incremental accumulator and inference.
 *
 * Integration model:
 *   - The search keeps a stack of Accumulators anchored at the root ply
 *     (acc_base, set by nnue_prepare_search). Every make_move inside the
 *     search is preceded by nnue_push_move()/nnue_push_null(), which writes
 *     slot[ply+1-base] as an update of the parent slot.
 *   - unmake needs no bookkeeping: child slots are overwritten by later
 *     pushes; parent slots are never mutated during pushes. The lazy
 *     bucket repair in nnue_eval() does mutate a slot, but only into the
 *     same values that position always yields — idempotent and safe.
 *   - Board copies outside the search (PV walk) never evaluate.
 *
 * RAVAGER_NNUE_VERIFY=1 enables an environment-gated self-check comparing
 * incremental accumulators against full rebuilds (debugging only).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "incbin.h"
#include "bitboard.h"
#include "nnue.h"

#ifdef EVALFILE
INCBIN(EmbeddedNet, EVALFILE);
#define HAVE_EMBEDDED_NET 1
#endif

Network nnue_net;
bool    nnue_loaded  = false;
bool    nnue_enabled = true;

int evaluate(Board *b);   /* HCE fallback (evaluate.c) */

/* ---- accumulator stack ------------------------------------------------ */

#define ACC_SLOTS (MAX_PLY + 16)

static Accumulator acc_stack[ACC_SLOTS];
static int         acc_base      = 0;
static bool        verify_mode   = false;
static long        verify_checks = 0, verify_fails = 0;

static inline int acc_idx(const Board *b) { return b->ply - acc_base; }

/* Leorik king-zone input buckets, indexed by the perspective-relative king
 * square (white king square as-is; black king square ^ 56). */
static const uint8_t leorik_bucket_map[64] = {
    0, 0, 1, 1, 1, 1, 0, 0,
    2, 2, 3, 3, 3, 3, 2, 2,
    2, 2, 3, 3, 3, 3, 2, 2,
    4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4,
};

static inline void king_state(const Board *b, int persp, int *bucket, int *mirror) {
    int ksq = lsb(b->pieces[persp][KING]);
    if (persp == BLACK) ksq ^= 56;
    *bucket = (nnue_net.in_buckets > 1) ? leorik_bucket_map[ksq] : 0;
    *mirror = (nnue_net.horiz_mirror && file_of(ksq) >= 4) ? 1 : 0;
}

/* ---- feature indexing -------------------------------------------------- */

static inline unsigned feat_idx(const Accumulator *acc, int persp,
                                int pt, int color, int sq) {
    if (nnue_net.format == NET_RAVAGER) {
        if (persp == BLACK) { color ^= 1; sq ^= 56; }
        return (unsigned)(pt * 128 + color * 64 + sq);
    }
    /* Leorik: [bucket][own 384 | enemy 384][type 64][square] */
    unsigned base = (unsigned)acc->kbucket[persp] * NN_INPUT;
    unsigned sub;
    int fsq;
    if (persp == WHITE) {
        sub = (color == WHITE) ? 0u : 384u;
        fsq = sq ^ (acc->kmirror[WHITE] ? 7 : 0);
    } else {
        sub = (color == WHITE) ? 384u : 0u;
        fsq = sq ^ (acc->kmirror[BLACK] ? 63 : 56);
    }
    return base + sub + (unsigned)pt * 64 + (unsigned)fsq;
}

/* idx_src supplies the bucket/mirror state used for indexing (parent for
 * removals, child for additions); acc receives the update. */
static inline void acc_add(Accumulator *acc, const Accumulator *idx_src,
                           int persp, int pt, int color, int sq) {
    const int16_t *w = nnue_net.ft_w +
        (size_t)feat_idx(idx_src, persp, pt, color, sq) * (size_t)nnue_net.hidden;
    int16_t *v = acc->vals[persp];
    for (int i = 0; i < nnue_net.hidden; i++) v[i] += w[i];
}

static inline void acc_sub(Accumulator *acc, const Accumulator *idx_src,
                           int persp, int pt, int color, int sq) {
    const int16_t *w = nnue_net.ft_w +
        (size_t)feat_idx(idx_src, persp, pt, color, sq) * (size_t)nnue_net.hidden;
    int16_t *v = acc->vals[persp];
    for (int i = 0; i < nnue_net.hidden; i++) v[i] -= w[i];
}

void nnue_refresh_accumulator(Accumulator *acc, const Board *b) {
    memcpy(acc->vals[WHITE], nnue_net.ft_b, sizeof(int16_t) * (size_t)nnue_net.hidden);
    memcpy(acc->vals[BLACK], nnue_net.ft_b, sizeof(int16_t) * (size_t)nnue_net.hidden);

    int bk, mi;
    king_state(b, WHITE, &bk, &mi);
    acc->kbucket[WHITE] = (uint8_t)bk; acc->kmirror[WHITE] = (uint8_t)mi;
    king_state(b, BLACK, &bk, &mi);
    acc->kbucket[BLACK] = (uint8_t)bk; acc->kmirror[BLACK] = (uint8_t)mi;

    for (int sq = 0; sq < 64; sq++) {
        if (b->piece_on[sq] == EMPTY_SQUARE) continue;
        int c = b->color_on[sq], p = b->piece_on[sq] % 6;
        for (int persp = WHITE; persp <= BLACK; persp++)
            acc_add(acc, acc, persp, p, c, sq);
    }
    acc->kvalid[WHITE] = acc->kvalid[BLACK] = 1;
}

/* Rebuild one perspective in place (lazy repair after a king crossed an
 * input-bucket boundary). */
static void refresh_perspective(Accumulator *acc, const Board *b, int persp) {
    memcpy(acc->vals[persp], nnue_net.ft_b, sizeof(int16_t) * (size_t)nnue_net.hidden);
    for (int sq = 0; sq < 64; sq++) {
        if (b->piece_on[sq] == EMPTY_SQUARE) continue;
        int c = b->color_on[sq], p = b->piece_on[sq] % 6;
        acc_add(acc, acc, persp, p, c, sq);
    }
    acc->kvalid[persp] = 1;
}

/* ---- delta updates ------------------------------------------------------ */

static void apply_move_to_acc(Accumulator *child, const Accumulator *parent,
                              const Board *b, Move m) {
    int from = move_from(m), to = move_to(m), flags = move_flags(m);
    int side = b->side, opp = side ^ 1;

    int piece = b->piece_on[from] % 6;

    /* captured piece leaves first (en passant pawn sits behind `to`) */
    if (flags == FLAG_EP) {
        int cap_sq = (side == WHITE) ? to - 8 : to + 8;
        for (int p = WHITE; p <= BLACK; p++)
            acc_sub(child, parent, p, PAWN, opp, cap_sq);
    } else if (b->piece_on[to] != EMPTY_SQUARE) {
        int captured = b->piece_on[to] % 6;
        for (int p = WHITE; p <= BLACK; p++)
            acc_sub(child, parent, p, captured, opp, to);
    }

    /* moving piece (promotion changes what lands on `to`) */
    int landed = piece;
    if (flags == FLAG_PROMO) {
        static const int promo_piece[4] = { KNIGHT, BISHOP, ROOK, QUEEN };
        landed = promo_piece[move_promo(m)];
    }
    for (int p = WHITE; p <= BLACK; p++) {
        acc_sub(child, parent, p, piece, side, from);
        acc_add(child, child,   p, landed, side, to);
    }

    /* castling moves the rook too */
    if (flags == FLAG_CASTLE) {
        int rook_from, rook_to;
        if      (to == G1) { rook_from = H1; rook_to = F1; }
        else if (to == C1) { rook_from = A1; rook_to = D1; }
        else if (to == G8) { rook_from = H8; rook_to = F8; }
        else               { rook_from = A8; rook_to = D8; }
        for (int p = WHITE; p <= BLACK; p++) {
            acc_sub(child, parent, p, ROOK, side, rook_from);
            acc_add(child, child,  p, ROOK, side, rook_to);
        }
    }
}

/* ---- search-side API ---------------------------------------------------- */

void nnue_prepare_search(const Board *b) {
    acc_base = b->ply;
    if (!nnue_loaded || !nnue_enabled) return;
    nnue_refresh_accumulator(&acc_stack[0], b);
}

void nnue_push_move(const Board *b, Move m) {
    if (!nnue_loaded || !nnue_enabled) return;
    int i = acc_idx(b);
    if (i < 0 || i >= ACC_SLOTS - 1) return;

    Accumulator *parent = &acc_stack[i];
    Accumulator *child  = &acc_stack[i + 1];
    *child = *parent;

    /* derive the child's king-bucket state before the board changes */
    int side  = b->side;
    int piece = b->piece_on[move_from(m)] % 6;

    int wksq = lsb(b->pieces[WHITE][KING]);
    int bksq = lsb(b->pieces[BLACK][KING]);
    if (side == WHITE && piece == KING) wksq = move_to(m);
    if (side == BLACK && piece == KING) bksq = move_to(m);

    child->kbucket[WHITE] = (nnue_net.in_buckets > 1) ? leorik_bucket_map[wksq] : 0;
    child->kmirror[WHITE] = (nnue_net.horiz_mirror && file_of(wksq) >= 4) ? 1 : 0;
    child->kbucket[BLACK] = (nnue_net.in_buckets > 1) ? leorik_bucket_map[bksq ^ 56] : 0;
    child->kmirror[BLACK] = (nnue_net.horiz_mirror && file_of(bksq ^ 56) >= 4) ? 1 : 0;

    /* a king crossing a bucket/mirror boundary invalidates that view */
    for (int p = WHITE; p <= BLACK; p++) {
        if (!parent->kvalid[p]) continue;              /* stays invalid */
        if (child->kbucket[p] != parent->kbucket[p] ||
            child->kmirror[p] != parent->kmirror[p])
            child->kvalid[p] = 0;
    }

    apply_move_to_acc(child, parent, b, m);
}

void nnue_push_null(const Board *b) {
    if (!nnue_loaded || !nnue_enabled) return;
    int i = acc_idx(b);
    if (i < 0 || i >= ACC_SLOTS - 1) return;
    acc_stack[i + 1] = acc_stack[i];   /* pieces unchanged: plain copy */
}

/* ---- inference ---------------------------------------------------------- */

static int nnue_evaluate_acc(Accumulator *acc, const Board *b) {
    int H = nnue_net.hidden;

    /* lazy repair of perspectives invalidated by king bucket changes */
    for (int p = WHITE; p <= BLACK; p++)
        if (!acc->kvalid[p]) refresh_perspective(acc, b, p);

    const int16_t *stm  = acc->vals[b->side];
    const int16_t *nstm = acc->vals[b->side ^ 1];

    if (nnue_net.format == NET_RAVAGER) {
        int32_t score = nnue_net.rav_out_bias;
        for (int i = 0; i < H; i++) {
            int a = stm[i];  if (a < 0) a = 0; else if (a > nnue_net.qa) a = nnue_net.qa;
            score += (int32_t)a * nnue_net.out_w[i];
        }
        for (int i = 0; i < H; i++) {
            int a = nstm[i]; if (a < 0) a = 0; else if (a > nnue_net.qa) a = nnue_net.qa;
            score += (int32_t)a * nnue_net.out_w[H + i];
        }
        /* accumulator head already yields the side-to-move POV score */
        return score / (nnue_net.qa * nnue_net.qb);
    }

    /* Leorik: SCReLU head with material output buckets */
    unsigned pc = popcount(b->occ_all);
    int div = (32 + nnue_net.out_buckets - 1) / nnue_net.out_buckets;
    int ob = (int)(pc - 2) / div;
    if (ob < 0) ob = 0;
    if (ob >= nnue_net.out_buckets) ob = nnue_net.out_buckets - 1;

    const int16_t *w_us   = nnue_net.out_w + (size_t)ob * 2 * H;
    const int16_t *w_nstm = w_us + H;

    int64_t sum = 0;
    for (int i = 0; i < H; i++) {
        int a = stm[i];  if (a < 0) a = 0; else if (a > 255) a = 255;
        sum += (int64_t)a * a * w_us[i];
    }
    for (int i = 0; i < H; i++) {
        int a = nstm[i]; if (a < 0) a = 0; else if (a > 255) a = 255;
        sum += (int64_t)a * a * w_nstm[i];
    }

    int out = (int)(sum / nnue_net.qa) + nnue_net.out_b[ob];
    /* accumulator head already yields the side-to-move POV score */
    return out * 400 / (nnue_net.qa * nnue_net.qb);
}

int nnue_eval(const Board *b) {
    if (!nnue_loaded || !nnue_enabled) return evaluate((Board*)b);

    int i = acc_idx(b);
    Accumulator *acc;
    static Accumulator scratch;
    if (i >= 0 && i < ACC_SLOTS) acc = &acc_stack[i];
    else {                       /* outside the search window: recompute */
        nnue_refresh_accumulator(&scratch, b);
        acc = &scratch;
    }

    /* repair pending perspectives first so verification sees final state */
    for (int p = WHITE; p <= BLACK; p++)
        if (!acc->kvalid[p]) refresh_perspective(acc, b, p);

    if (verify_mode) {
        Accumulator fresh;
        nnue_refresh_accumulator(&fresh, b);
        verify_checks++;
        size_t vals_bytes = sizeof(int16_t) * (size_t)nnue_net.hidden * 2;
        if (memcmp(fresh.vals, acc->vals, vals_bytes) != 0) {
            verify_fails++;
            if (verify_fails <= 3) {
                fprintf(stderr, "NNUE VERIFY MISMATCH #%ld at ply %d\n",
                        verify_fails, b->ply);
                for (int k = 0; k < nnue_net.hidden; k++)
                    for (int p = WHITE; p <= BLACK; p++)
                        if (fresh.vals[p][k] != acc->vals[p][k])
                            fprintf(stderr, "  %s[%d]: inc=%d fresh=%d\n",
                                    p ? "B" : "W", k,
                                    acc->vals[p][k], fresh.vals[p][k]);
            }
        }
        if ((verify_checks & 0xFFFFF) == 0 && verify_checks)
            fprintf(stderr, "NNUE VERIFY: %ld checks, %ld fails\n",
                    verify_checks, verify_fails);
    }

    return nnue_evaluate_acc(acc, b);
}

int nnue_evaluate_board(const Board *b) {
    if (!nnue_loaded || !nnue_enabled) return evaluate((Board*)b);
    Accumulator acc;
    nnue_refresh_accumulator(&acc, b);
    return nnue_evaluate_acc(&acc, b);
}


/* ---- loading ------------------------------------------------------------ */

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error "Big-endian hosts are not supported by the net loaders yet."
#endif

static void free_net(void) {
    free(nnue_net.ft_w);
    free(nnue_net.ft_b);
    free(nnue_net.out_w);
    free(nnue_net.out_b);
    memset(&nnue_net, 0, sizeof(nnue_net));
    nnue_loaded = false;
}

static bool load_ravager_stream(FILE *f) {
    uint32_t magic = 0, version = 0;
    if (fread(&magic, 4, 1, f) != 1 || fread(&version, 4, 1, f) != 1 ||
        magic != NNUE_MAGIC || version != NNUE_VERSION)
        return false;

    free_net();
    nnue_net.format       = NET_RAVAGER;
    nnue_net.hidden       = 256;
    nnue_net.in_buckets   = 1;
    nnue_net.out_buckets  = 1;
    nnue_net.horiz_mirror = false;
    nnue_net.qa = NNUE_QA;
    nnue_net.qb = NNUE_QB;

    const size_t FT = (size_t)NN_INPUT * 256;      /* 768*256 weights */
    nnue_net.ft_w  = malloc(FT * 2);
    nnue_net.ft_b  = malloc(256 * 2);
    nnue_net.out_w = malloc((size_t)2 * 256 * 2);
    nnue_net.out_b = NULL;
    if (!nnue_net.ft_w || !nnue_net.ft_b || !nnue_net.out_w) { free_net(); return false; }

    int32_t bias32 = 0;
    if (fread(nnue_net.ft_w,  2, FT, f) != FT ||
        fread(nnue_net.ft_b,  2, 256, f) != 256 ||
        fread(nnue_net.out_w, 2, 512, f) != 512 ||
        fread(&bias32, 4, 1, f) != 1) { free_net(); return false; }
    nnue_net.rav_out_bias = bias32;

    nnue_loaded = true;
    return true;
}

/* Filename pattern "<H>HL-S-<K>io<O>-*.nnue" (Leorik convention). */
static bool leorik_dims_from_name(const char *path, int *H, int *K, int *O) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *hl = strstr(base, "HL-S-");
    if (!hl || hl == base) return false;
    char *end;
    long h = strtol(base, &end, 10);
    if (h <= 0 || end != hl || h > NN_HIDDEN_MAX) return false;
    long k = strtol(hl + 5, &end, 10);
    if (k <= 0 || k > 16 || strncmp(end, "io", 2) != 0) return false;
    long o = strtol(end + 2, &end, 10);
    if (o <= 0 || o > NN_OUTBUCK_MAX) return false;
    *H = (int)h; *K = (int)k; *O = (int)o;
    return true;
}

static bool load_leorik_stream(FILE *f, const char *path) {
    int H = 640, K = 5, O = 8;                     /* current Leorik default */
    bool named = leorik_dims_from_name(path, &H, &K, &O);

    size_t ft_cnt  = (size_t)K * NN_INPUT * H;
    size_t ob_cnt  = (size_t)O;
    size_t ow_cnt  = (size_t)O * 2 * H;

    /* headerless: validate against the expected byte count when the
     * filename did not give us dimensions */
    if (!named && fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        long need = (long)(ft_cnt + H + ow_cnt + ob_cnt) * 2;
        rewind(f);
        if (sz < need) {
            fprintf(stderr,
                "info string cannot infer Leorik net dims from '%s'\n", path);
            return false;
        }
    } else rewind(f);

    free_net();
    nnue_net.format       = NET_LEORIK;
    nnue_net.hidden       = H;
    nnue_net.in_buckets   = K;
    nnue_net.out_buckets  = O;
    nnue_net.horiz_mirror = true;
    nnue_net.qa = 255;
    nnue_net.qb = 64;

    nnue_net.ft_w  = malloc(ft_cnt * 2);
    nnue_net.ft_b  = malloc((size_t)H * 2);
    nnue_net.out_w = malloc(ow_cnt * 2);
    nnue_net.out_b = malloc(ob_cnt * sizeof(int32_t));
    if (!nnue_net.ft_w || !nnue_net.ft_b || !nnue_net.out_w || !nnue_net.out_b) {
        free_net(); return false;
    }

    int16_t *ob16 = malloc(ob_cnt * 2);
    if (!ob16) { free_net(); return false; }

    bool ok = fread(nnue_net.ft_w,  2, ft_cnt, f) == ft_cnt &&
              fread(nnue_net.ft_b,  2, (size_t)H, f) == (size_t)H &&
              fread(nnue_net.out_w, 2, ow_cnt, f) == ow_cnt &&
              fread(ob16, 2, ob_cnt, f) == ob_cnt;
    if (ok)
        for (size_t i = 0; i < ob_cnt; i++) nnue_net.out_b[i] = ob16[i];
    free(ob16);
    if (!ok) { free_net(); return false; }

    nnue_loaded = true;
    return true;
}

/* Detect format and dispatch. Ravager nets carry a magic header; Leorik
 * nets are headerless int16 dumps recognised by filename/size. Both entry
 * points (file + embedded blob) share this path. */
static bool nnue_load_any(FILE *f, const char *name_hint) {
    const char *disp = (name_hint && *name_hint) ? name_hint : "<embedded>";

    uint32_t magic = 0;
    if (fread(&magic, 4, 1, f) == 1 && magic == NNUE_MAGIC) {
        rewind(f);
        if (!load_ravager_stream(f)) {
            fprintf(stderr, "info string invalid Ravager net %s\n", disp);
            return false;
        }
        fprintf(stderr,
            "info string loaded Ravager net '%s' (768x%dx2->1)\n",
            disp, nnue_net.hidden);
        return true;
    }

    rewind(f);
    if (!load_leorik_stream(f, disp)) {
        fprintf(stderr, "info string unrecognised NNUE file %s\n", disp);
        return false;
    }
    fprintf(stderr,
        "info string loaded Leorik net '%s' (%dx768x%d -> x2 -> %d buckets)\n",
        disp, nnue_net.in_buckets, nnue_net.hidden, nnue_net.out_buckets);
    return true;
}

bool nnue_load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "info string cannot open NNUE file %s\n", path); return false; }
    bool ok = nnue_load_any(f, path);
    fclose(f);
    return ok;
}

bool nnue_load_embedded(void) {
#ifdef HAVE_EMBEDDED_NET
    FILE *f = fmemopen((void*)gEmbeddedNetData, gEmbeddedNetSize, "rb");
    if (!f) { fprintf(stderr, "info string cannot read embedded NNUE\n"); return false; }
    bool ok = nnue_load_any(f, NULL);   /* no filename: dims inferred by size */
    fclose(f);
    return ok;
#else
    return false;
#endif
}

static void nnue_verify_report(void) {
    fprintf(stderr, "NNUE VERIFY FINAL: %ld checks, %ld fails\n",
            verify_checks, verify_fails);
}

__attribute__((constructor)) static void nnue_read_env(void) {
    verify_mode = getenv("RAVAGER_NNUE_VERIFY") != NULL;
    if (verify_mode) atexit(nnue_verify_report);
}
