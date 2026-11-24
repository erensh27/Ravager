/* Deterministic search, move-selection and board regressions; no match play. */
#include "../src/search.c"
#include "bench.h"
#include <assert.h>
#include <stddef.h>

static ThreadData *position(const char *fen) {
    Board b;
    assert(parse_fen(&b, fen));
    ThreadData *t = threads[0];
    reset_thread(t, &b, NULL, 0);
    limits = (SearchLimits){.infinite = true};
    setup_time(&b);
    atomic_store(&stop_flag, false);
    clear_thread(t);
    tt_clear();
    compute_threats(&t->board, &t->stack[STACK_OFFSET]);
    return t;
}

static void test_picker(void) {
    ThreadData *t = position("k3r3/8/8/8/8/2N5/P2B4/R3K3 w - - 0 1");
    Stack *ss = &t->stack[STACK_OFFSET];
    MoveList all;
    generate_moves(&t->board, &all);
    bool expected[UINT16_MAX + 1] = {false};
    int count = 0;
    Move first = NO_MOVE;
    for (int i = 0; i < all.count; i++)
        if (move_is_legal(&t->board, all.moves[i])) {
            expected[all.moves[i]] = true;
            first = all.moves[i];
            count++;
        }
    assert(count > 3);
    Picker p;
    picker_init(&p, &t->board, first, NO_MOVE, 10000, true);
    p.skip_quiets = true;  /* Neither this nor SEE may discard an evasion. */
    Move m;
    while ((m = picker_next(&p, t, ss)) != NO_MOVE) {
        assert(expected[m]);
        expected[m] = false;
        count--;
    }
    assert(count == 0);

    t = position("6k1/8/4p3/3p4/3Q4/8/8/6K1 w - - 0 1");
    ss = &t->stack[STACK_OFFSET];
    Move capture = parse_move(&t->board, "d4d5");
    assert(!see_ge(&t->board, capture, MAIN_SEE_BOUND));
    for (int late = 0; late < 2; late++) {
        picker_init(&p, &t->board, NO_MOVE, NO_MOVE, MAIN_SEE_BOUND, false);
        p.skip_quiets = !late;
        bool found = false;
        while ((m = picker_next(&p, t, ss)) != NO_MOVE) {
            if (p.skip_quiets) assert(move_is_noisy(&t->board, m));
            if (m == capture) found = true;
            p.skip_quiets = true;
        }
        assert(found);
    }
    picker_init(&p, &t->board, capture, NO_MOVE, 0, true);
    while ((m = picker_next(&p, t, ss)) != NO_MOVE)
        assert(see_ge(&t->board, m, 0));  /* Includes the TT move. */

    t = position("r6k/1P6/8/8/8/8/8/6K1 w - - 0 1");
    assert(captured_type(&t->board, parse_move(&t->board, "b7a8q")) == ROOK);
    assert(captured_type(&t->board, parse_move(&t->board, "b7b8q")) == PAWN);
    picker_init(&p, &t->board, NO_MOVE, NO_MOVE, QS_SEE_BOUND, true);
    int promotions = 0;
    while ((m = picker_next(&p, t, &t->stack[STACK_OFFSET])) != NO_MOVE)
        promotions += move_is_promo(m) && move_to(m) == A8;
    assert(promotions == 4);
}

static void test_history_context(void) {
    const struct { const char *fen, *move; bool check, tactical; } cases[] = {
        {STARTPOS_FEN, "e2e4", false, false},
        {"7k/8/8/4p3/3P4/8/8/6K1 w - - 0 1", "d4e5", false, true},
        {"4r2k/8/8/8/8/8/3B4/4K3 w - - 0 1", "d2e3", true, false},
        {"7k/8/8/8/8/4n3/3B4/5K2 w - - 0 1", "d2e3", true, true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ThreadData *t = position(cases[i].fen);
        Stack *ss = &t->stack[STACK_OFFSET];
        Move m = parse_move(&t->board, cases[i].move);
        assert(m != NO_MOVE && move_is_legal(&t->board, m));
        assert((t->board.checkers != 0) == cases[i].check);
        int piece = t->board.piece_on[move_from(m)], to = move_to(m);
        ss->reduction = 8192;
        do_move(t, ss, m, cases[i].tactical);
        assert(ss->reduction == 0);
        update_cont(ss + 1, BLACK * 6 + KING, H8, 4, true);
        assert(t->cont[cases[i].check][cases[i].tactical][piece][to][BLACK * 6 + KING][H8] > 0);
        for (int c = 0; c < 2; c++)
            for (int cap = 0; cap < 2; cap++)
                if (c != cases[i].check || cap != cases[i].tactical)
                    assert(t->cont[c][cap][piece][to][BLACK * 6 + KING][H8] == 0);
        undo_move(t, m);
    }

    ThreadData *t = position(STARTPOS_FEN);
    Stack *ss = &t->stack[STACK_OFFSET];
    ss->ply = 6;
    assert(qsearch(t, ss, mate_in(6), MATE_SCORE, false) == mate_in(6));
    assert(atomic_load(&t->nodes) == 0);
}

static void test_draws(void) {
    ThreadData *t = position(STARTPOS_FEN);
    Stack *ss = &t->stack[STACK_OFFSET];
    t->board.half_move_clock = 20;
    t->key_root = 8;
    memset(t->keys, 0, sizeof t->keys);
    t->keys[4] = t->board.hash;
    assert(!is_draw(t, ss));  /* A single pre-root match is not threefold. */
    t->keys[0] = t->board.hash;
    assert(is_draw(t, ss));
    ss->plies_from_null = 3;
    assert(!is_draw(t, ss));
    ss->plies_from_null = 20;
    memset(t->keys, 0, sizeof t->keys);
    ss->ply = 5;
    t->keys[9] = t->board.hash;
    assert(is_draw(t, ss));  /* A cycle strictly inside the search is enough. */
    t = position("7k/6Q1/5K2/8/8/8/8/8 b - - 100 1");
    ss = &t->stack[STACK_OFFSET];
    assert(!is_draw(t, ss));
    assert(qsearch(t, ss, -INFINITY_SCORE, INFINITY_SCORE, true) == -MATE_SCORE);
    t = position("7k/8/5KQ1/8/8/8/8/8 b - - 100 1");
    assert(is_draw(t, &t->stack[STACK_OFFSET]));
}

static void test_tt(void) {
    const uint64_t hash = UINT64_C(0x923456789abcdef0);
    const int scores[] = {123, -456, SCORE_NONE, MATE_SCORE - 12,
                          -MATE_SCORE + 12, TB_WIN_SCORE - 12, -TB_WIN_SCORE + 12};
    TTHit hit;
    for (size_t i = 0; i < sizeof scores / sizeof scores[0]; i++) {
        tt_clear();
        tt_store(hash, 8, NO_MOVE, scores[i], 52, BOUND_EXACT, 12, true);
        assert(tt_probe(hash, 8, 0, &hit) && hit.score == scores[i]);
        int delta = scores[i] == SCORE_NONE ? 0 : scores[i] >= TB_WIN_BOUND ? 6
                  : scores[i] <= -TB_WIN_BOUND ? -6 : 0;
        assert(tt_probe(hash, 2, 0, &hit) && hit.score == scores[i] + delta);
        assert(hit.eval == 52 && hit.depth == 12 && hit.pv);
    }
    tt_store(hash, 0, NO_MOVE, MATE_SCORE - 50, 0, BOUND_EXACT, 12, false);
    assert(tt_probe(hash, 0, 60, &hit) && hit.score == EVAL_LIMIT);
    tt_store(hash, 0, NO_MOVE, -TB_WIN_SCORE + 50, 0, BOUND_EXACT, 12, false);
    assert(tt_probe(hash, 0, 60, &hit) && hit.score == -EVAL_LIMIT);
}

static void test_qsearch(void) {
    ThreadData *t = position(STARTPOS_FEN);
    Stack *ss = &t->stack[STACK_OFFSET];
    int score = qsearch(t, ss, -INFINITY_SCORE, INFINITY_SCORE, true);
    TTHit hit;
    assert(tt_probe(t->board.hash, 0, 0, &hit));
    assert(hit.bound == BOUND_EXACT && hit.score == score);
    tt_clear();
    int cutoff = qsearch(t, ss, score - 101, score - 100, false);
    assert(cutoff >= score - 100);
    assert(tt_probe(t->board.hash, 0, 0, &hit) && hit.bound == BOUND_LOWER);

    t = position("7k/8/5KQ1/8/8/8/8/8 w - - 0 1");
    ss = &t->stack[STACK_OFFSET];
    assert(negamax(t, ss, -INFINITY_SCORE, INFINITY_SCORE, 2, false, true) >= MATE_BOUND);
    assert(t->corr_pawn[t->board.pawn_key % CORR_SIZE][WHITE] == 0);
}

static void test_upcoming(void) {
    const char *fens[] = {
        "k3r3/8/8/8/8/8/4N3/4K3 w - - 0 1",
        "k2r4/8/8/8/8/8/4N3/4K3 w - - 0 1"
    };
    for (int i = 0; i < 2; i++) {
        Board b;
        assert(parse_fen(&b, fens[i]));
        Move m = parse_move(&b, "e2c3");
        uint64_t history[4] = {0};
        make_move(&b, m);
        history[0] = b.hash;
        unmake_move(&b, m);
        history[3] = b.hash;
        assert(board_upcoming_repetition(&b, history, 3, 3) == (i == 1));
    }
}

static void test_see(void) {
    ThreadData *t = position("4k3/4n3/8/3p4/2B5/8/8/4R1K1 w - - 0 1");
    assert(see_ge(&t->board, parse_move(&t->board, "c4d5"), 1));
    t = position("5k2/4n3/8/3p4/2B5/8/8/4R1K1 w - - 0 1");
    assert(!see_ge(&t->board, parse_move(&t->board, "c4d5"), 1));
}

static void test_voting(void) {
    search_set_threads(3);
    Move queen = encode_move(48, 56, FLAG_PROMO, PROMO_QUEEN);
    Move knight = encode_move(48, 56, FLAG_PROMO, PROMO_KNIGHT);
    for (int i = 0; i < 3; i++) {
        threads[i]->completed = 12;
        threads[i]->best_pv_len = 1;
        threads[i]->best_pv[0] = i ? knight : queen;
    }
    threads[0]->score = MATE_SCORE - 5;
    threads[1]->score = -MATE_SCORE + 1;
    threads[2]->score = 0;
    assert(select_best_thread() == threads[0]);
    threads[0]->score = -MATE_SCORE + 5;
    threads[2]->score = -MATE_SCORE + 10;
    assert(select_best_thread() == threads[2]);
    threads[0]->score = 110;
    threads[1]->score = threads[2]->score = 100;
    threads[1]->completed = threads[2]->completed = 13;
    assert(select_best_thread()->best_pv[0] == knight);
    search_set_threads(1);
}

static void test_searchmoves(void) {
    Board b;
    assert(parse_fen(&b, STARTPOS_FEN));
    ThreadData *t = threads[0];
    limits = (SearchLimits){0};
    limits.searchmoves[0] = parse_move(&b, "e2e4");
    limits.searchmoves[1] = parse_move(&b, "d2d4");
    limits.searchmoves_count = 2;
    reset_thread(t, &b, NULL, 0);
    assert(t->root_move_count == 2);
    assert(root_move_allowed(t, limits.searchmoves[0]));
    assert(root_move_allowed(t, limits.searchmoves[1]));
    assert(!root_move_allowed(t, parse_move(&b, "g1f3")));
    t->root_skip[t->root_skip_count++] = limits.searchmoves[0];
    assert(!root_move_allowed(t, limits.searchmoves[0]));
    t->root_skip_count = 0;

    limits.searchmoves[0] = parse_move(&b, "e2e5");   /* illegal move */
    limits.searchmoves_count = 1;
    reset_thread(t, &b, NULL, 0);
    assert(t->root_move_count == 20);                 /* falls back to all legal */
}

static void test_multipv(void) {
    Board b;
    assert(parse_fen(&b, STARTPOS_FEN));
    SearchLimits lim;
    memset(&lim, 0, sizeof lim);
    lim.depth = 8;
    lim.searchmoves[0] = parse_move(&b, "g1f3");
    lim.searchmoves[1] = parse_move(&b, "b1c3");
    lim.searchmoves_count = 2;
    search_set_multipv(4);
    SearchResult res;
    search_run(&b, NULL, 0, &lim, &res);
    assert(res.depth == 8);
    assert(res.best == lim.searchmoves[0] || res.best == lim.searchmoves[1]);
    search_set_multipv(1);
    assert(search_get_multipv() == 1);
}

static void check_tree(Board *b, int depth) {
    Board original = *b;
    MoveList all, evasions;
    generate_moves(b, &all);
    if (b->checkers) {
        generate_evasions(b, &evasions);
        int legal = 0;
        for (int i = 0; i < all.count; i++) {
            if (!move_is_legal(b, all.moves[i])) continue;
            int matches = 0;
            for (int j = 0; j < evasions.count; j++) matches += all.moves[i] == evasions.moves[j];
            assert(matches == 1);
            legal++;
        }
        assert(legal == evasions.count);
    }
    if (!depth) return;
    for (int i = 0; i < all.count; i++) {
        Move m = all.moves[i];
        if (!move_is_legal(b, m)) continue;
        make_move(b, m);
        check_tree(b, depth - 1);
        unmake_move(b, m);
        assert(memcmp(b, &original, offsetof(Board, undo_stack)) == 0);
    }
}

static Bitboard slider_reference(int sq, Bitboard occ, int diagonal) {
    Bitboard result = 0;
    for (int dr = -1; dr <= 1; dr++)
        for (int df = -1; df <= 1; df++) {
            if ((!dr && !df) || ((dr != 0 && df != 0) != diagonal)) continue;
            int r = sq / 8 + dr, f = sq % 8 + df;
            while (r >= 0 && r < 8 && f >= 0 && f < 8) {
                Bitboard bit = 1ULL << (r * 8 + f);
                result |= bit;
                if (occ & bit) break;
                r += dr;
                f += df;
            }
        }
    return result;
}

static void test_sliders(void) {
    for (int sq = 0; sq < 64; sq++)
        for (int diagonal = 0; diagonal < 2; diagonal++) {
            Bitboard edges = ((0xffULL | (0xffULL << 56)) & ~(0xffULL << (sq / 8 * 8))) |
                             ((FILE_A | FILE_H) & ~(FILE_A << (sq % 8)));
            Bitboard mask = slider_reference(sq, 0, diagonal) & ~edges, occ = 0;
            do {
                Bitboard expected = slider_reference(sq, occ, diagonal);
                assert((diagonal ? bishop_attacks(sq, occ) : rook_attacks(sq, occ)) == expected);
                occ = (occ - mask) & mask;
            } while (occ);
        }
}

static void test_board(void) {
    static const struct { const char *fen; int depth; uint64_t nodes; } cases[] = {
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5, 4865609},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603},
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 4, 43238},
        {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 3, 9467},
        {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3, 62379},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Board b;
        assert(parse_fen(&b, cases[i].fen));
        assert(perft(&b, cases[i].depth) == cases[i].nodes);
        check_tree(&b, 2);
    }
    for (int i = 0; i < 960; i++) {
        Board b;
        assert(board_start960(&b, i));
        check_tree(&b, 1);
    }
}

int main(void) {
    board_init_all();
    search_init();
    search_set_threads(1);
    tt_alloc(16);
    search_verbose = 0;
    test_picker();
    test_history_context();
    test_draws();
    test_tt();
    test_qsearch();
    test_upcoming();
    test_see();
    test_voting();
    test_searchmoves();
    test_multipv();
    test_sliders();
    test_board();
    puts("Search, TT, SEE, evasion and perft regressions passed");
    return 0;
}
