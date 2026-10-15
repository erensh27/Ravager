/* uci.c — UCI protocol front-end, perft, bench, main() */

#include "bitboard.h"
#include "tt.h"
#include "search.h"
#include "nnue.h"
#include "tb_syzygy.h"
#include "diagnostics.h"
#include <pthread.h>

bool is_insufficient_material(const Board *b);  /* evaluate.c */

int move_overhead_ms = 20;
static pthread_t uci_thread;
static bool uci_thread_valid, uci_job_active, uci_exit;
static pthread_mutex_t uci_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t uci_cond=PTHREAD_COND_INITIALIZER;
static char *uci_job;
static void finish_search(bool stop) {
    pthread_mutex_lock(&uci_mutex);
    if(stop && uci_job_active) search_stop();
    while(uci_job_active) pthread_cond_wait(&uci_cond,&uci_mutex);
    pthread_mutex_unlock(&uci_mutex);
}

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
    if (b->castling_rights & CASTLE_WK) { *o++ = uci_chess960 ? 'A' + file_of(b->castle_rook[0]) : 'K'; any = 1; }
    if (b->castling_rights & CASTLE_WQ) { *o++ = uci_chess960 ? 'A' + file_of(b->castle_rook[1]) : 'Q'; any = 1; }
    if (b->castling_rights & CASTLE_BK) { *o++ = uci_chess960 ? 'a' + file_of(b->castle_rook[2]) : 'k'; any = 1; }
    if (b->castling_rights & CASTLE_BQ) { *o++ = uci_chess960 ? 'a' + file_of(b->castle_rook[3]) : 'q'; any = 1; }
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
            int nl = 0;
            int mover = b->side;
            for (int j = 0; j < ml.count; j++) {
                make_move(b, ml.moves[j]);
                if (!is_in_check(b, mover)) nl++;
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
            search_new_job();
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

/* ---- position parsing ---- */
static void parse_position(const char *line) {
    const char *p = line;

    if (strncmp(p,"startpos960 ",12)==0) {
        board_start960(&main_board,atoi(p+12));p+=12;
        while(*p && *p!=' ') p++;
    } else if (strncmp(p, "startpos", 8) == 0) {
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
static void run_go(const char *line) {
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
        int moves_left = movestogo > 0 ? movestogo : 25;
        if (!movestogo) {
            /* Phase-aware estimate: fewer moves needed in endgame */
            int phase = main_board.game_phase;
            moves_left = 18 + phase / 4;   /* 18 (endgame) to 24 (opening) */
            if (game_hist_len >= 80) moves_left -= 4;   /* late game: speed up */
            if (moves_left < 10) moves_left = 10;
        }
        int base = my_time / moves_left;
        int soft = base + my_inc * 3 / 4 - move_overhead_ms;
        int hard = soft * 4 + my_inc / 2;
        /* Hard cap: never spend more than 75% of remaining clock on one move */
        if (hard > my_time * 3 / 4) hard = my_time * 3 / 4;
        if (hard < soft) hard = soft;
        search_soft_ms = soft > 1 ? soft : 1;
        search_hard_ms = hard;
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
            printf("bestmove %s\n", move_to_uci(&main_board,only));
            fflush(stdout);
            return;
        }
    }

    /* Tablebase root probe: DTZ-optimal move when within range */
    if (syzygy_available()) {
        Move tb_move = syzygy_probe_root(&main_board);
        if (tb_move != NO_MOVE) {
            printf("info string syzygy root move (DTZ-optimal)\n");
            printf("bestmove %s\n", move_to_uci(&main_board,tb_move));
            fflush(stdout);
            return;
        }
    }

    search_iterative_deepening(&main_board);

    printf("bestmove %s", move_to_uci(&main_board,search_root_best));
    /* ponder move from TT */
    if (search_root_best != NO_MOVE) {
        Board t = main_board;
        make_move(&t, search_root_best);
        Move pm = tt_probe_move(t.hash);
        if (pm != NO_MOVE) printf(" ponder %s", move_to_uci(&t,pm));
    }
    printf("\n");
    fflush(stdout);
}

/* Persistent coordinator retains the primary worker's learned histories. */
static void *go_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&uci_mutex);
    for(;;) {
        while(!uci_job && !uci_exit) pthread_cond_wait(&uci_cond,&uci_mutex);
        if(uci_exit) break;
        char *line=uci_job;uci_job=NULL;
        pthread_mutex_unlock(&uci_mutex);
        if(!strcmp(line,"__reset")) search_reset_tables();
        else run_go(line);
        free(line);
        pthread_mutex_lock(&uci_mutex);
        uci_job_active=false;pthread_cond_broadcast(&uci_cond);
    }
    pthread_mutex_unlock(&uci_mutex);return NULL;
}
static void parse_go(const char *line) {
    finish_search(true);search_new_job();
    char *copy=malloc(strlen(line)+1);
    if(!copy) return;
    strcpy(copy,line);
    if(!uci_thread_valid) {
        if(pthread_create(&uci_thread,NULL,go_thread,NULL)) {run_go(copy);free(copy);return;}
        uci_thread_valid=true;
    }
    pthread_mutex_lock(&uci_mutex);
    uci_job=copy;uci_job_active=true;
    pthread_cond_broadcast(&uci_cond);pthread_mutex_unlock(&uci_mutex);
}

static void print_options(void) {
    printf("id name Ravager Fury\n");
    printf("id author %s\n", ENGINE_AUTHOR);
    printf("option name Hash type spin default %d min 8 max 16384\n", DEFAULT_TT_MB);
    printf("option name Threads type spin default 1 min 1 max 4096\n");
    printf("option name UCI_Chess960 type check default false\n");
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
        if (hash_mb < 8) hash_mb = 8;
        if (hash_mb > 16384) hash_mb = 16384;
        tt_alloc(hash_mb);
    } else if (sscanf(line, "setoption name Threads value %63s", value) == 1) {
        search_threads=atoi(value);
        if(search_threads<1) search_threads=1;
        if(search_threads>4096) search_threads=4096;
    } else if(option_value(line,"UCI_Chess960",value,sizeof value)) {
        uci_chess960=!strcmp(value,"true");
    } else if (sscanf(line, "setoption name MoveOverhead value %63s", value) == 1) {
        move_overhead_ms = atoi(value);
    } else if (option_value(line, "EvalFile", value, sizeof(value))) {
        if (*value && strcmp(value, "<empty>") != 0) {
            if (!nnue_load_file(value)) printf("info string NNUE network load failed; prior network retained\n");
        }
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
        run_bench(&main_board);
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

        /* Mutating board/options waits for the old search before any writes. */
        if(strcmp(line,"isready") && strcmp(line,"uci") && strcmp(line,"stop") &&
           strncmp(line,"go",2)) finish_search(true);
        if (strcmp(line, "uci") == 0) print_options();
        else if (strcmp(line, "isready") == 0) { printf("readyok\n"); fflush(stdout); }
        else if (strcmp(line, "ucinewgame") == 0) {
            tt_clear();
            if(uci_thread_valid) {parse_go("__reset");finish_search(false);}
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

    finish_search(true);
    if(uci_thread_valid) {
        pthread_mutex_lock(&uci_mutex);uci_exit=true;pthread_cond_broadcast(&uci_cond);pthread_mutex_unlock(&uci_mutex);
        pthread_join(uci_thread,NULL);
    }
    return 0;
}
