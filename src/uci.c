/* uci.c — UCI protocol front-end, perft, bench, main() */

#include "bitboard.h"
#include "tt.h"
#include "search.h"
#include "nnue.h"
#include "tb_syzygy.h"

bool is_insufficient_material(const Board *b);  /* evaluate.c */
bool nnue_load_embedded(void);                  /* nnue.c     */

int move_overhead_ms = 20;

static Board main_board;

/* Hash of the game position `ply` plies before the current one (undo stack
 * stores pre-move keys). */
static uint64_t game_history_hash_at(const Board *b, int ply) {
    if (ply < 0 || ply >= b->game_ply) return 0;
    return b->undo_stack[ply].zobrist_key;
}
static int hash_mb = DEFAULT_TT_MB;
static uint64_t game_hist[2048];
static int game_hist_len = 0;

/* ---- FEN writer ---- */
static void board_to_fen(const Board *b, char *out) {
    char *o = out;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            int sq = r * 8 + f;
            if (b->piece_on[sq] == EMPTY_SQUARE) { empty++; continue; }
            if (empty) { *o++ = '0' + empty; empty = 0; }
            char c = "PNBRQK"[b->piece_on[sq] % 6];
            if (b->color_on[sq] == BLACK) c += 32;
            *o++ = c;
        }
        if (empty) *o++ = '0' + empty;
        if (r) *o++ = '/';
    }
    *o++ = ' ';
    *o++ = (b->side == WHITE) ? 'w' : 'b';
    *o++ = ' ';
    int any = 0;
    if (b->castling_rights & CASTLE_WK) { *o++ = 'K'; any = 1; }
    if (b->castling_rights & CASTLE_WQ) { *o++ = 'Q'; any = 1; }
    if (b->castling_rights & CASTLE_BK) { *o++ = 'k'; any = 1; }
    if (b->castling_rights & CASTLE_BQ) { *o++ = 'q'; any = 1; }
    if (!any) *o++ = '-';
    *o++ = ' ';
    if (b->ep_square == NO_SQUARE) *o++ = '-';
    else { *o++ = 'a' + file_of(b->ep_square); *o++ = '1' + rank_of(b->ep_square); }
    strcpy(o, " 0 1");
}

/* ---- self-play data generation for texel tuning ---- */
static uint64_t dg_rng_state;
static uint64_t dg_rand(void) {
    uint64_t x = dg_rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return dg_rng_state = x;
}

static void run_datagen(int ngames, int movetime_ms, uint64_t seed, const char *outfile) {
    search_verbose = 0;
    FILE *f = fopen(outfile, "w");
    if (!f) { fprintf(stderr, "cannot open %s\n", outfile); return; }
    dg_rng_state = seed ? seed : 0x12345678DEADBEEFULL;
    Board *b = &main_board;
    char game_fens[512][128];
    int total_pos = 0;

    search_max_depth = 100;

    for (int g = 0; g < ngames; g++) {
        parse_fen(b, STARTPOS_FEN);
        tt_clear();

        /* random opening: 4-10 random legal plies */
        int n_rand = 4 + (int)(dg_rand() % 7);
        for (int i = 0; i < n_rand; i++) {
            MoveList ml;
            generate_moves(b, &ml);
            int legal_idx[MAX_MOVES], nl = 0;
            int mover = b->side;
            for (int j = 0; j < ml.count; j++) {
                make_move(b, ml.moves[j]);
                if (!is_in_check(b, mover)) legal_idx[nl++] = j;
                unmake_move(b, ml.moves[j]);
            }
            if (nl == 0) break;
            make_move(b, ml.moves[legal_idx[dg_rand() % nl]]);
        }

        /* play the game out with shallow fixed-time searches */
        int result = -1;   /* 1 white, 0 black, 5 draw */
        int recorded = 0;
        for (int ply = 0; ply < 300; ply++) {
            if (is_insufficient_material(b) || b->half_move_clock >= 100) { result = 5; break; }
            MoveList ml;
            generate_moves(b, &ml);
            int legal[MAX_MOVES], nl = 0;
            int mover = b->side;
            for (int j = 0; j < ml.count; j++) {
                make_move(b, ml.moves[j]);
                if (!is_in_check(b, mover)) legal[nl++] = j;
                unmake_move(b, ml.moves[j]);
            }
            if (nl == 0) { result = is_in_check(b, mover) ? (mover == WHITE ? 0 : 1) : 5; break; }

            /* detect repetition draw within the game */
            bool rep = false;
            for (int i = b->game_ply - 4; i >= 0 && i >= b->game_ply - b->half_move_clock; i -= 2)
                if (game_history_hash_at(b, i) == b->hash) { rep = true; break; }
            if (rep) { result = 5; break; }

            if (recorded < 512) {
                board_to_fen(b, game_fens[recorded++]);
            }

            search_soft_ms = movetime_ms;
            search_hard_ms = movetime_ms + 2;
            search_iterative_deepening(b);
            Move best = search_root_best;
            if (best == NO_MOVE || parse_move(b, move_to_str(best)) == NO_MOVE) { result = 5; break; }
            make_move(b, best);
        }
        if (result == -1) result = 5;

        double score = (result == 5) ? 0.5 : (result == 1 ? 1.0 : 0.0);
        for (int i = 0; i < recorded; i++)
            fprintf(f, "%s %.1f\n", game_fens[i], score);
        total_pos += recorded;
        if ((g + 1) % 10 == 0) { fprintf(stderr, "games %d/%d, positions %d\n", g + 1, ngames, total_pos); fflush(stderr); }
    }
    fclose(f);
    printf("Datagen complete: %d positions -> %s\n", total_pos, outfile);
    fflush(stdout);
}

/* ---- perft ---- */
static uint64_t perft(Board *b, int depth) {
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

static void perft_divide(Board *b, int depth) {
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

/* ---- position parsing ---- */
static void parse_position(const char *line) {
    const char *p = line;

    if (strncmp(p, "startpos", 8) == 0) {
        parse_fen(&main_board, STARTPOS_FEN);
        p += 8;
    } else if (strncmp(p, "fen ", 4) == 0) {
        p += 4;
        parse_fen(&main_board, p);
        while (*p && strncmp(p, "moves", 5) != 0) p++;
    }

    game_hist_len = 0;
    if ((p = strstr(p, "moves")) != NULL) {
        p += 5;
        while (*p == ' ') p++;
        while (*p) {
            Move m = parse_move(&main_board, p);
            if (m == NO_MOVE) break;
            game_hist[game_hist_len++] = main_board.hash;
            make_move(&main_board, m);
            while (*p && *p != ' ') p++;
            while (*p == ' ') p++;
        }
    }
    search_set_game_history(game_hist, game_hist_len);
}

/* ---- go parsing ---- */
static void parse_go(const char *line) {
    int wtime = 0, btime = 0, winc = 0, binc = 0, movestogo = 0, movetime = 0, depth_limit = 100;
    bool infinite = false;

    const char *p = line;
    while (*p) {
        if      (sscanf(p, "wtime %d", &wtime) == 1) {}
        else if (sscanf(p, "btime %d", &btime) == 1) {}
        else if (sscanf(p, "winc %d",  &winc)  == 1) {}
        else if (sscanf(p, "binc %d",  &binc)  == 1) {}
        else if (sscanf(p, "movestogo %d", &movestogo) == 1) {}
        else if (sscanf(p, "movetime %d", &movetime) == 1) {}
        else if (sscanf(p, "depth %d", &depth_limit) == 1) {}
        else if (strncmp(p, "infinite", 8) == 0) infinite = true;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }

    int my_time = (main_board.side == WHITE) ? wtime : btime;
    int my_inc  = (main_board.side == WHITE) ? winc  : binc;

    if (infinite) {
        search_soft_ms = 3600000;
        search_hard_ms = 3600000;
    } else if (movetime > 0) {
        search_soft_ms = movetime - move_overhead_ms;
        search_hard_ms = movetime - move_overhead_ms / 2;
    } else if (my_time > 0) {
        int moves_left = movestogo > 0 ? movestogo : 30;
        if (game_hist_len >= 60 && !movestogo) moves_left = 20;  /* long game, speed up */
        int base = my_time / moves_left;
        search_soft_ms = base + my_inc * 4 / 5 - move_overhead_ms;
        search_hard_ms = search_soft_ms * 5;
        if (search_hard_ms > my_time * 4 / 5) search_hard_ms = my_time * 4 / 5;
    } else {
        search_soft_ms = 1000;
        search_hard_ms = 3000;
    }
    if (search_soft_ms < 1) search_soft_ms = 1;
    if (search_hard_ms < search_soft_ms) search_hard_ms = search_soft_ms;

    search_max_depth = depth_limit;

    /* Single legal move: play it instantly */
    {
        MoveList ml;
        Board tmp = main_board;
        generate_moves(&tmp, &ml);
        int mover = tmp.side;
        int legal = 0;
        Move only = NO_MOVE;
        for (int i = 0; i < ml.count; i++) {
            make_move(&tmp, ml.moves[i]);
            if (!is_in_check(&tmp, mover)) { legal++; only = ml.moves[i]; }
            unmake_move(&tmp, ml.moves[i]);
            if (legal > 1) break;
        }
        if (legal == 1) {
            printf("bestmove %s\n", move_to_str(only));
            fflush(stdout);
            return;
        }
    }

    /* Tablebase root probe: DTZ-optimal move when within range */
    if (syzygy_available()) {
        Move tb_move = syzygy_probe_root(&main_board);
        if (tb_move != NO_MOVE) {
            printf("info string syzygy root move (DTZ-optimal)\n");
            printf("bestmove %s\n", move_to_str(tb_move));
            fflush(stdout);
            return;
        }
    }

    search_iterative_deepening(&main_board);

    printf("bestmove %s", move_to_str(search_root_best));
    /* ponder move from TT */
    if (search_root_best != NO_MOVE) {
        Board t = main_board;
        make_move(&t, search_root_best);
        Move pm = tt_probe_move(t.hash);
        if (pm != NO_MOVE) printf(" ponder %s", move_to_str(pm));
    }
    printf("\n");
    fflush(stdout);
}

static void print_options(void) {
    printf("id name %s %s\n", ENGINE_NAME, ENGINE_VERSION);
    printf("id author %s\n", ENGINE_AUTHOR);
    printf("option name Hash type spin default %d min 1 max 65536\n", DEFAULT_TT_MB);
    printf("option name MoveOverhead type spin default %d min 0 max 5000\n", move_overhead_ms);
    printf("option name Ponder type check default false\n");
    printf("option name Use NNUE type check default true\n");
    printf("option name EvalFile type string default <empty>\n");
    printf("option name SyzygyPath type string default <empty>\n");
    printf("option name SyzygyProbeLimit type spin default 6 min 1 max 7\n");
    printf("option name Syzygy50MoveRule type check default true\n");
    printf("uciok\n");
    fflush(stdout);
}

/* Extract "name ... value ..." where the value may contain spaces. */
static bool option_value(const char *line, const char *name, char *out, size_t outsz) {
    char pat[128];
    snprintf(pat, sizeof(pat), "setoption name %s value ", name);
    const char *p = strstr(line, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    snprintf(out, outsz, "%s", p);
    /* trim trailing whitespace */
    size_t n = strlen(out);
    while (n > 0 && (out[n-1] == ' ' || out[n-1] == '\t')) out[--n] = 0;
    return true;
}

static void handle_setoption(const char *line) {
    char value[1024];
    if (sscanf(line, "setoption name Hash value %63s", value) == 1) {
        hash_mb = atoi(value);
        if (hash_mb < 1) hash_mb = 1;
        if (hash_mb > 65536) hash_mb = 65536;
        tt_alloc(hash_mb);
    } else if (sscanf(line, "setoption name MoveOverhead value %63s", value) == 1) {
        move_overhead_ms = atoi(value);
    } else if (option_value(line, "EvalFile", value, sizeof(value))) {
        if (*value && strcmp(value, "<empty>") != 0) nnue_load_file(value);
        else { nnue_loaded = false; nnue_load_embedded(); }
    } else if (option_value(line, "SyzygyPath", value, sizeof(value))) {
        syzygy_init(value);
    } else if (sscanf(line, "setoption name SyzygyProbeLimit value %63s", value) == 1) {
        syzygy_probe_limit = atoi(value);
        if (syzygy_probe_limit < 0) syzygy_probe_limit = 0;
        if (syzygy_probe_limit > 7) syzygy_probe_limit = 7;
    } else if (sscanf(line, "setoption name Syzygy50MoveRule value %63s", value) == 1) {
        syzygy_50move_rule = (strcmp(value, "true") == 0);
    } else if (strstr(line, "setoption name Use NNUE")) {
        if (option_value(line, "Use NNUE", value, sizeof(value)))
            nnue_enabled = (strcmp(value, "true") == 0);
    }
}

static void print_board(void) {
    printf("\n  +---+---+---+---+---+---+---+---+\n");
    for (int r = 7; r >= 0; r--) {
        printf("%d |", r + 1);
        for (int f = 0; f < 8; f++) {
            int sq = r * 8 + f;
            char c = ' ';
            if (main_board.piece_on[sq] != EMPTY_SQUARE) {
                c = "PNBRQK"[main_board.piece_on[sq] % 6];
                if (main_board.color_on[sq] == BLACK) c += 32;
            }
            printf(" %c |", c);
        }
        printf("\n  +---+---+---+---+---+---+---+---+\n");
    }
    printf("    a   b   c   d   e   f   g   h\n");
    printf("Side: %s  Hash: %016llx  Eval: %d\n",
           main_board.side == WHITE ? "white" : "black",
           (unsigned long long)main_board.hash, nnue_evaluate_board(&main_board));
    fflush(stdout);
}

static void run_bench(void) {
    const char *fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P3/2NP1N2/PPPQ1PPP/R4RK1 w - - 0 10",
    };
    search_max_depth = 8;
    uint64_t total = 0;
    for (unsigned i = 0; i < sizeof(fens)/sizeof(fens[0]); i++) {
        search_set_game_history(game_hist, 0);
        parse_fen(&main_board, fens[i]);
        search_soft_ms = 60000;
        search_hard_ms = 60000;
        search_iterative_deepening(&main_board);
        total += search_nodes;
    }
    printf("Bench total nodes: %llu\n", (unsigned long long)total);
}

int main(int argc, char *argv[]) {
    board_init_all();
    init_lmr_table();
    tt_alloc(hash_mb);
    search_reset_tables();
    nnue_load_embedded();     /* no-op when built without EVALFILE */
    {
        const char *env_net = getenv("RAVAGER_EVALFILE");
        if (env_net && *env_net) nnue_load_file(env_net);
    }
    parse_fen(&main_board, STARTPOS_FEN);

    if (argc > 1 && strcmp(argv[1], "bench") == 0) {
        run_bench();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "datagen") == 0) {
        int ngames = argc > 2 ? atoi(argv[2]) : 50;
        int mvt    = argc > 3 ? atoi(argv[3]) : 12;
        uint64_t seed = argc > 4 ? strtoull(argv[4], NULL, 10) : 1;
        const char *out = argc > 5 ? argv[5] : "training.txt";
        run_datagen(ngames, mvt, seed, out);
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "perft") == 0) {
        uint64_t n = perft(&main_board, atoi(argv[2]));
        printf("Perft(%d) = %llu\n", atoi(argv[2]), (unsigned long long)n);
        return 0;
    }

    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;

        if (strcmp(line, "uci") == 0) print_options();
        else if (strcmp(line, "isready") == 0) { printf("readyok\n"); fflush(stdout); }
        else if (strcmp(line, "ucinewgame") == 0) {
            tt_clear();
            search_reset_tables();
            parse_fen(&main_board, STARTPOS_FEN);
            game_hist_len = 0;
        }
        else if (strncmp(line, "setoption", 9) == 0) handle_setoption(line);
        else if (strncmp(line, "position", 8) == 0) parse_position(line + 9);
        else if (strncmp(line, "go", 2) == 0) parse_go(line + 3);
        else if (strcmp(line, "stop") == 0) search_stop();
        else if (strncmp(line, "perft", 5) == 0) perft_divide(&main_board, atoi(line + 6));
        else if (strncmp(line, "fen ", 4) == 0) parse_fen(&main_board, line + 4);
        else if (strcmp(line, "d") == 0) print_board();
        else if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) break;
    }

    return 0;
}
