/* bench.c — perft and the built-in fixed-depth benchmark. */
#include "bench.h"
#include "search.h"
#include "tt.h"
#include <time.h>

uint64_t perft(Board *b, int depth) {
    if (depth == 0) return 1;
    MoveList moves;
    generate_moves(b, &moves);
    uint64_t nodes = 0;
    for (int i = 0; i < moves.count; i++) {
        Move m = moves.moves[i];
        if (!move_is_legal(b, m)) continue;
        if (depth == 1) { nodes++; continue; }
        make_move(b, m);
        nodes += perft(b, depth - 1);
        unmake_move(b, m);
    }
    return nodes;
}

void perft_divide(Board *b, int depth) {
    if (depth < 1) depth = 1;
    MoveList moves;
    generate_moves(b, &moves);
    uint64_t total = 0;
    for (int i = 0; i < moves.count; i++) {
        Move m = moves.moves[i];
        if (!move_is_legal(b, m)) continue;
        make_move(b, m);
        uint64_t count = perft(b, depth - 1);
        unmake_move(b, m);
        printf("%s: %llu\n", move_to_uci(b, m), (unsigned long long)count);
        total += count;
    }
    printf("Total: %llu\n", (unsigned long long)total);
    fflush(stdout);
}

static const char *bench_fens[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P3/2NP1N2/PPPQ1PPP/R4RK1 w - - 0 10",
    "r1bq1rk1/pp2bppp/2n1pn2/3p4/2PP4/2N1PN2/PP1B1PPP/R2QKB1R w KQ - 0 8",
    "2r3k1/pp3ppp/4p3/3pP3/3P2Q1/2P5/P4PPP/R5K1 b - - 0 22",
    "r1bqkb1r/pp3ppp/2n1pn2/2pp4/3P4/2PBPN2/PP3PPP/RNBQK2R w KQkq - 0 6",
    "6k1/5pp1/7p/8/3R4/6P1/r4P1P/6K1 w - - 0 35",
    "r2q1rk1/1b2bppp/p2ppn2/1p6/3NP3/1BN1Q3/PPP2PPP/R4RK1 w - - 0 13",
    "8/8/1p1k4/p1p5/P1P2K2/1P6/8/8 w - - 0 50",
    "2kr3r/ppp2ppp/2n1b3/2b1q3/4P3/2NB1N2/PPP2PPP/R2Q1RK1 b - - 0 11",
    "4rrk1/pp1n3p/3q2pQ/2p1pb2/2PP4/2P3N1/P2B2PP/4RRK1 b - - 7 19",
};

/* Fixed-depth search over a small suite; prints total nodes and speed. */
void run_bench(int depth) {
    if (depth <= 0) depth = 13;
    int saved = search_verbose;
    search_verbose = 0;
    uint64_t total = 0;
    struct timespec a, z;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (size_t i = 0; i < sizeof bench_fens / sizeof bench_fens[0]; i++) {
        Board b;
        parse_fen(&b, bench_fens[i]);
        tt_clear();
        search_clear();
        SearchLimits lim;
        memset(&lim, 0, sizeof lim);
        lim.depth = depth;
        SearchResult res;
        search_run(&b, NULL, 0, &lim, &res);
        total += res.nodes;
    }
    clock_gettime(CLOCK_MONOTONIC, &z);
    double secs = (double)(z.tv_sec - a.tv_sec) + (double)(z.tv_nsec - a.tv_nsec) / 1e9;
    search_verbose = saved;
    printf("%llu nodes %.0f nps\n", (unsigned long long)total, secs > 0 ? total / secs : 0.0);
    fflush(stdout);
}
