/* Compare incremental accumulators and every available kernel with fresh
 * scalar inference on a deterministic position tree, including lazy/null paths. */
#include "../nnue/inference.c"
#include "nnue.h"

static int checked;

static void test_rows(void) {
    _Alignas(64) int16_t src[WIDTH], expected[WIDTH], actual[WIDTH];
    uint32_t add[] = {0, 17, 1023, 11000}, sub[] = {7, 8000, 9000, 10000};
    for (int j = 0; j < WIDTH; j++) src[j] = j & 1 ? INT16_MIN : INT16_MAX;
    for (int na = 0; na <= 4; na++)
        for (int ns = 0; ns <= 4; ns++) {
            RowOps o = {add, sub, na, ns};
            for (int aux = 0; aux < 2; aux++) {
                void (*scalar)(int16_t *, const int16_t *, const RowOps *) = aux ? aux_rows_scalar : psqt_rows_scalar;
                scalar(expected, src, &o);
#if defined(__x86_64__) || defined(__i386__)
                for (int tier = 0; tier < 2; tier++) {
                    if (!__builtin_cpu_supports("avx2")) continue;
                    if (tier && !(__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw"))) continue;
                    void (*kernel)(int16_t *, const int16_t *, const RowOps *) = tier
                        ? (aux ? aux_rows_avx512 : psqt_rows_avx512)
                        : (aux ? aux_rows_avx2 : psqt_rows_avx2);
                    kernel(actual, src, &o);
                    assert(!memcmp(actual, expected, sizeof actual));
                    memcpy(actual, src, sizeof actual);
                    kernel(actual, actual, &o);
                    assert(!memcmp(actual, expected, sizeof actual));
                }
#elif defined(__aarch64__)
                void (*kernel)(int16_t *, const int16_t *, const RowOps *) = aux ? aux_rows_neon : psqt_rows_neon;
                kernel(actual, src, &o);
                assert(!memcmp(actual, expected, sizeof actual));
                memcpy(actual, src, sizeof actual);
                kernel(actual, actual, &o);
                assert(!memcmp(actual, expected, sizeof actual));
#endif
            }
        }
}

static void test_pair_features(void) {
    Board b;
    assert(parse_fen(&b, STARTPOS_FEN));
    Frame f = {0};
    memcpy(f.squares, b.piece_on, 64);
    f.pawns = b.pieces[0][PAWN] | b.pieces[1][PAWN];
    for (int view = 0; view < 4; view++) {
        int p = view / 2;
        f.view[p] = (p ? 56 : 0) ^ ((view & 1) ? 7 : 0);
        for (unsigned mask = 0; mask < 256; mask++) {
            Bitboard changed = (Bitboard)mask << 8 | (Bitboard)(255 ^ mask) << 48;
            bool expected[4560] = {false};
            int count = 0;
            for (int s = 0; s < 64; s++)
                for (int to = s + 1; to < 64; to++) {
                    Bitboard pair = (1ULL << s) | (1ULL << to);
                    if ((f.pawns & pair) != pair || !(pair & changed) || abs((s & 7) - (to & 7)) > 1) continue;
                    unsigned a = (s ^ f.view[p]) - 8 + (f.squares[s] / 6 ^ p) * 48;
                    unsigned v = (to ^ f.view[p]) - 8 + (f.squares[to] / 6 ^ p) * 48;
                    unsigned hi = a > v ? a : v, lo = a < v ? a : v;
                    expected[hi * (hi - 1) / 2 + lo] = true;
                    count++;
                }
            uint32_t ids[CAP];
            int n = pawn_pairs(&f, changed, p, ids, 0);
            assert(n == count);
            for (int i = 0; i < n; i++) {
                assert(ids[i] < 4560 && expected[ids[i]]);
                expected[ids[i]] = false;
            }
        }
    }
}

static void verify(Board *b) {
    Frame scratch, exact;
    Frame *f = sync_frame(b, &scratch, false);
    build_frame(&exact, b, NULL, false);
    assert(!memcmp(f->psqt, exact.psqt, sizeof f->psqt));
    assert(!memcmp(f->thr, exact.thr, sizeof f->thr));
    int score = forward_scalar(&exact, b);
    assert(net_eval(b, false) == score);
    assert(net_eval(b, false) == score);  /* Repeated reads use the cached head. */
    assert(net_eval(b, true) == score);
#if defined(__x86_64__) || defined(__i386__)
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
        assert(forward_avx2(f, b) == score);
    if (__builtin_cpu_supports("avxvnni") && __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
        assert(forward_avxvnni(f, b) == score);
    if (__builtin_cpu_supports("avx512vnni") && __builtin_cpu_supports("avx512bw") &&
        __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("fma"))
        assert(forward_vnni(f, b) == score);
#elif defined(__aarch64__)
    assert(forward_neon(f, b) == score);
#if defined(__ARM_FEATURE_DOTPROD)
    assert(forward_neon_dot(f, b) == score);
#elif defined(__linux__)
    if (getauxval(AT_HWCAP) & HWCAP_ASIMDDP)
        assert(forward_neon_dot(f, b) == score);
#endif
#endif
    checked++;
}

static void visit(Board *b, int depth) {
    if ((b->ply & 1) == 0 || !depth) verify(b);
    if (!b->checkers) {
        net_push_null(b);
        make_null_move(b);
        verify(b);
        unmake_null_move(b);
    }
    if (!depth) return;
    MoveList moves;
    generate_moves(b, &moves);
    for (int i = 0, n = 0; i < moves.count && n < 12; i++) {
        Move m = moves.moves[i];
        if (!move_is_legal(b, m)) continue;
        n++;
        net_push(b);
        make_move(b, m);
        visit(b, depth - 1);
        unmake_move(b, m);
    }
    verify(b);
}

static void test_groups(void) {
#if defined(__x86_64__) || defined(__i386__)
    if (!__builtin_cpu_supports("avx2")) return;
#endif
    _Alignas(32) uint8_t x[WIDTH];
    uint16_t idx[256];
    for (unsigned mask = 0; mask < 256; mask++) {
        memset(x, 0, sizeof x);
        for (int g = 0; g < 256; g++)
            if (mask & (1u << (g % 8))) x[g * 4 + g % 4] = 127;
        int n = nonzero_groups(x, idx), expected = 0;
        for (int g = 0; g < 256; g++)
            if (mask & (1u << (g % 8))) assert(idx[expected++] == g);
        assert(n == expected);
    }
}

static void test_null_alias(void) {
    Board b;
    assert(parse_fen(&b, STARTPOS_FEN));
    net_prepare(&b);
    verify(&b);
    net_push_null(&b);
    make_null_move(&b);
    Frame scratch;
    assert(sync_frame(&b, &scratch, false) == &frames[0]);
    verify(&b);
    unmake_null_move(&b);

    /* A null move can also follow an unevaluated normal move. */
    Move m = parse_move(&b, "e2e4");
    net_push(&b);
    make_move(&b, m);
    assert(!frames[1].ready);
    net_push_null(&b);
    make_null_move(&b);
    verify(&b);
    unmake_null_move(&b);
    verify(&b);
    unmake_move(&b, m);
    verify(&b);
}

static void test_reload(void) {
    FILE *fp = fopen("nets/Ravager_NET.nnue", "rb");
    assert(fp);
    unsigned char *bytes = malloc(NET_BYTES);
    assert(bytes && fread(bytes, 1, NET_BYTES, fp) == NET_BYTES);
    fclose(fp);
    /* Change an in-memory copy only, to expose stale cache use after a load. */
    int16_t value;
    memcpy(&value, bytes + BIAS_OFFSET, sizeof value);
    value = wrap(value + 16);
    memcpy(bytes + BIAS_OFFSET, &value, sizeof value);
    assert(net_load(bytes, NET_BYTES));
    Board b;
    assert(parse_fen(&b, STARTPOS_FEN));
    net_prepare(&b);
    verify(&b);
    free(bytes);
    assert(nnue_load_embedded());
    net_prepare(&b);
    verify(&b);
}

int main(void) {
    board_init_all();
    assert(nnue_load_embedded());
    test_rows();
    test_pair_features();
    test_groups();
    test_null_alias();
    const char *fens[] = {
        STARTPOS_FEN,
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r6k/1P6/8/8/8/8/8/6K1 w - - 0 1",
        "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1",
        "4k3/8/8/8/8/8/8/R1K2R2 w FA - 0 1",
        "4k3/8/8/8/8/8/8/R4KR1 w GA - 0 1",
        "k3r3/8/8/8/8/2N5/P2B4/R3K3 w - - 0 1",
    };
    for (size_t i = 0; i < sizeof fens / sizeof fens[0]; i++) {
        Board b;
        assert(parse_fen(&b, fens[i]));
        net_prepare(&b);
        visit(&b, 3);
    }
    test_reload();
    printf("NNUE: %d incremental/fresh/kernel comparisons passed (%s)\n", checked, net_tier);
    return 0;
}
