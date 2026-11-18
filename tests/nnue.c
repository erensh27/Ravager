/* Compare incremental accumulators and every available kernel with fresh
 * scalar inference on a deterministic position tree, including lazy/null paths. */
#include "../nnue/inference.c"
#include "nnue.h"

static int checked;

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
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
        assert(forward_avx2(f, b) == score);
    if (__builtin_cpu_supports("avx512vnni") && __builtin_cpu_supports("avx512bw") &&
        __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("fma"))
        assert(forward_vnni(f, b) == score);
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
    if (!__builtin_cpu_supports("avx2")) return;
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
