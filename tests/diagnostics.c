#include "diagnostics.h"
#include "search.h"

/* ---- perft ---- */
uint64_t perft(Board *b, int depth) {
    if (depth == 0) return 1;
    MoveList moves;
    generate_moves(b, &moves);
    uint64_t nodes = 0;
    int mover = b->side;
    for (int i = 0; i < moves.count; i++) {
        make_move(b, moves.moves[i]);
        if (!is_in_check(b, mover)) nodes += perft(b, depth - 1);
        unmake_move(b, moves.moves[i]);
    }
    return nodes;
}

void perft_divide(Board *b, int depth) {
    MoveList moves;
    generate_moves(b, &moves);
    uint64_t total = 0;
    int mover = b->side;
    for (int i = 0; i < moves.count; i++) {
        Move m = moves.moves[i];
        make_move(b, m);
        if (is_in_check(b, mover)) { unmake_move(b, m); continue; }
        uint64_t count = perft(b, depth - 1);
        unmake_move(b, m);
        printf("%s: %llu\n", move_to_str(m), (unsigned long long)count);
        total += count;
    }
    printf("Total: %llu\n", (unsigned long long)total);
}


void run_bench(Board *board) {
    const char *fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P3/2NP1N2/PPPQ1PPP/R4RK1 w - - 0 10",
    };
    search_max_depth = 8;
    uint64_t total = 0;
    for (unsigned i = 0; i < sizeof(fens)/sizeof(fens[0]); i++) {
        search_set_game_history(NULL, 0);
        parse_fen(board, fens[i]);
        search_soft_ms = 60000;
        search_hard_ms = 60000;
        search_new_job(); search_iterative_deepening(board);
        total += search_nodes;
    }
    printf("Bench total nodes: %llu\n", (unsigned long long)total);
}
