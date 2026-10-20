/* uci.c — UCI protocol front-end and main(). */

#define _POSIX_C_SOURCE 200809L

#include "bitboard.h"
#include "tt.h"
#include "search.h"
#include "nnue.h"
#include "tb_syzygy.h"
#include "bench.h"
#include <pthread.h>
#include <strings.h>


static int hash_mb = DEFAULT_TT_MB;

/* ---- game state ---- */
#define GAME_HISTORY 1024
static Board main_board;
static uint64_t game_hist[GAME_HISTORY];   /* keys of positions before the current one */
static int game_hist_len = 0;

/* Appends a played move; positions before an irreversible move can never
 * repeat, so the history restarts there and stays bounded. */
static void play_move(Board *b, Move m) {
    if (game_hist_len == GAME_HISTORY) {
        memmove(game_hist, game_hist + GAME_HISTORY / 2, sizeof(uint64_t) * (GAME_HISTORY / 2));
        game_hist_len = GAME_HISTORY / 2;
    }
    game_hist[game_hist_len++] = b->hash;
    make_move(b, m);
    b->ply = 0;
    if (b->half_move_clock == 0) game_hist_len = 0;
}

static Move parse_legal_move(Board *b, const char *s) {
    Move m = parse_move(b, s);
    if (m == NO_MOVE || !move_is_pseudo_legal(b, m) || !move_is_legal(b, m)) return NO_MOVE;
    return m;
}

/* ---- background search thread ---- */
static pthread_t search_thread;
static bool search_thread_valid, job_active, uci_exit;
static pthread_mutex_t job_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
static SearchLimits job_limits;
static Board job_board;
static uint64_t job_hist[GAME_HISTORY];
static int job_hist_len;
static bool job_pending;

static void *search_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&job_mutex);
    for (;;) {
        while (!job_pending && !uci_exit) pthread_cond_wait(&job_cond, &job_mutex);
        if (uci_exit) break;
        job_pending = false;
        pthread_mutex_unlock(&job_mutex);
        SearchResult result;
        search_run(&job_board, job_hist, job_hist_len, &job_limits, &result);
        pthread_mutex_lock(&job_mutex);
        job_active = false;
        pthread_cond_broadcast(&job_cond);
    }
    pthread_mutex_unlock(&job_mutex);
    return NULL;
}

/* Waits for the running search; optionally asks it to stop first. */
static void wait_search(bool stop) {
    pthread_mutex_lock(&job_mutex);
    if (stop && job_active) search_stop();
    while (job_active) pthread_cond_wait(&job_cond, &job_mutex);
    pthread_mutex_unlock(&job_mutex);
}

static void start_search(const SearchLimits *lim) {
    wait_search(true);
    pthread_mutex_lock(&job_mutex);
    if (!search_thread_valid) {
        if (pthread_create(&search_thread, NULL, search_worker, NULL)) {
            pthread_mutex_unlock(&job_mutex);
            SearchResult result;
            search_run(&main_board, game_hist, game_hist_len, lim, &result);
            return;
        }
        search_thread_valid = true;
    }
    job_board = main_board;
    memcpy(job_hist, game_hist, sizeof(uint64_t) * (size_t)game_hist_len);
    job_hist_len = game_hist_len;
    job_limits = *lim;
    job_pending = true;
    job_active = true;
    pthread_cond_broadcast(&job_cond);
    pthread_mutex_unlock(&job_mutex);
}

/* ---- FEN writer ---- */
static void board_to_fen(const Board *b, char *out) {
    char *o = out;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            int sq = r * 8 + f;
            if (b->piece_on[sq] == EMPTY_SQUARE) { empty++; continue; }
            if (empty) { *o++ = (char)('0' + empty); empty = 0; }
            char c = "PNBRQK"[b->piece_on[sq] % 6];
            if (b->color_on[sq] == BLACK) c = (char)(c + 32);
            *o++ = c;
        }
        if (empty) *o++ = (char)('0' + empty);
        if (r) *o++ = '/';
    }
    *o++ = ' ';
    *o++ = (b->side == WHITE) ? 'w' : 'b';
    *o++ = ' ';
    int any = 0;
    if (b->castling_rights & CASTLE_WK) { *o++ = uci_chess960 ? (char)('A' + file_of(b->castle_rook[0])) : 'K'; any = 1; }
    if (b->castling_rights & CASTLE_WQ) { *o++ = uci_chess960 ? (char)('A' + file_of(b->castle_rook[1])) : 'Q'; any = 1; }
    if (b->castling_rights & CASTLE_BK) { *o++ = uci_chess960 ? (char)('a' + file_of(b->castle_rook[2])) : 'k'; any = 1; }
    if (b->castling_rights & CASTLE_BQ) { *o++ = uci_chess960 ? (char)('a' + file_of(b->castle_rook[3])) : 'q'; any = 1; }
    if (!any) *o++ = '-';
    *o++ = ' ';
    if (b->ep_square == NO_SQUARE) *o++ = '-';
    else { *o++ = (char)('a' + file_of(b->ep_square)); *o++ = (char)('1' + rank_of(b->ep_square)); }
    sprintf(o, " %d %d", b->half_move_clock, b->full_move_number);
}

/* ---- position ---- */
static void parse_position(char *line) {
    char *p = line;
    while (*p == ' ') p++;
    if (!strncmp(p, "startpos960 ", 12)) {
        if (!board_start960(&main_board, atoi(p + 12))) return;
        p += 12;
    } else if (!strncmp(p, "startpos", 8)) {
        parse_fen(&main_board, STARTPOS_FEN);
        p += 8;
    } else if (!strncmp(p, "fen ", 4)) {
        p += 4;
        char *moves = strstr(p, " moves");
        if (moves) *moves = 0;
        bool ok = parse_fen(&main_board, p);
        if (moves) { *moves = ' '; p = moves; } else p += strlen(p);
        if (!ok) { printf("info string invalid FEN ignored\n"); fflush(stdout); return; }
    } else {
        return;
    }
    main_board.ply = 0;
    game_hist_len = 0;
    char *moves = strstr(p, "moves");
    if (!moves) return;
    char *save = NULL;
    for (char *tok = strtok_r(moves + 5, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        Move m = parse_legal_move(&main_board, tok);
        if (m == NO_MOVE) break;
        play_move(&main_board, m);
    }
}

/* ---- go ---- */
static void parse_go(char *line) {
    SearchLimits lim;
    memset(&lim, 0, sizeof lim);
    char *save = NULL;
    for (char *tok = strtok_r(line, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        char *arg;
#define NEXT_INT() ((arg = strtok_r(NULL, " \t", &save)) ? atoll(arg) : 0)
        if      (!strcmp(tok, "wtime"))     lim.time[WHITE] = (int)NEXT_INT();
        else if (!strcmp(tok, "btime"))     lim.time[BLACK] = (int)NEXT_INT();
        else if (!strcmp(tok, "winc"))      lim.inc[WHITE] = (int)NEXT_INT();
        else if (!strcmp(tok, "binc"))      lim.inc[BLACK] = (int)NEXT_INT();
        else if (!strcmp(tok, "movestogo")) lim.movestogo = (int)NEXT_INT();
        else if (!strcmp(tok, "movetime"))  lim.movetime = (int)NEXT_INT();
        else if (!strcmp(tok, "depth"))     lim.depth = (int)NEXT_INT();
        else if (!strcmp(tok, "nodes"))     lim.nodes = (uint64_t)NEXT_INT();
        else if (!strcmp(tok, "mate"))      lim.mate = (int)NEXT_INT();
        else if (!strcmp(tok, "infinite"))  lim.infinite = true;
        else if (!strcmp(tok, "ponder"))    lim.ponder = true;
#undef NEXT_INT
    }
    if (lim.time[WHITE] < 0) lim.time[WHITE] = 0;
    if (lim.time[BLACK] < 0) lim.time[BLACK] = 0;
    start_search(&lim);
}

/* ---- options ---- */
static void print_options(void) {
    printf("id name %s\n", ENGINE_FULL_NAME);
    printf("id author %s\n", ENGINE_AUTHOR);
    printf("option name Hash type spin default %d min 1 max 65536\n", DEFAULT_TT_MB);
    printf("option name Threads type spin default 1 min 1 max 1024\n");
    printf("option name Clear Hash type button\n");
    printf("option name Ponder type check default false\n");
    printf("option name UCI_Chess960 type check default false\n");
    printf("option name MoveOverhead type spin default %d min 0 max 5000\n", move_overhead_ms);
    printf("option name Use NNUE type check default true\n");
    printf("option name EvalFile type string default <empty>\n");
    printf("option name SyzygyPath type string default <empty>\n");
    printf("option name SyzygyProbeLimit type spin default 6 min 0 max 7\n");
    printf("option name Syzygy50MoveRule type check default true\n");
    printf("uciok\n");
    fflush(stdout);
}

/* "setoption name <name> [value <value>]"; names are case-insensitive. */
static void handle_setoption(const char *line) {
    const char *n = strstr(line, "name ");
    if (!n) return;
    n += 5;
    char name[128] = {0}, value[1024] = {0};
    const char *v = strstr(n, " value ");
    size_t nl = v ? (size_t)(v - n) : strlen(n);
    if (nl >= sizeof name) nl = sizeof name - 1;
    memcpy(name, n, nl);
    while (nl && (name[nl - 1] == ' ' || name[nl - 1] == '\t')) name[--nl] = 0;
    if (v) {
        v += 7;
        while (*v == ' ') v++;
        snprintf(value, sizeof value, "%s", v);
        size_t vl = strlen(value);
        while (vl && (value[vl - 1] == ' ' || value[vl - 1] == '\t')) value[--vl] = 0;
    }
    bool on = !strcasecmp(value, "true");
    if (!strcasecmp(name, "Hash")) {
        int mb = atoi(value);
        hash_mb = mb < 1 ? 1 : mb > 65536 ? 65536 : mb;
        if (!tt_alloc(hash_mb)) printf("info string hash allocation failed; previous table kept\n");
    } else if (!strcasecmp(name, "Threads")) {
        int n_threads = atoi(value);
        search_set_threads(n_threads < 1 ? 1 : n_threads > 1024 ? 1024 : n_threads);
    } else if (!strcasecmp(name, "Clear Hash")) {
        tt_clear();
    } else if (!strcasecmp(name, "UCI_Chess960")) {
        uci_chess960 = on;
    } else if (!strcasecmp(name, "MoveOverhead") || !strcasecmp(name, "Move Overhead")) {
        int mo = atoi(value);
        move_overhead_ms = mo < 0 ? 0 : mo > 5000 ? 5000 : mo;
    } else if (!strcasecmp(name, "Use NNUE")) {
        nnue_enabled = on;
    } else if (!strcasecmp(name, "EvalFile")) {
        if (*value && strcmp(value, "<empty>")) {
            if (!nnue_load_file(value)) printf("info string network load failed; previous network kept\n");
        } else {
            nnue_load_embedded();
        }
    } else if (!strcasecmp(name, "SyzygyPath")) {
        syzygy_init(value);
    } else if (!strcasecmp(name, "SyzygyProbeLimit")) {
        int l = atoi(value);
        syzygy_probe_limit = l < 0 ? 0 : l > 7 ? 7 : l;
    } else if (!strcasecmp(name, "Syzygy50MoveRule")) {
        syzygy_50move_rule = on;
    }
    fflush(stdout);
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
                if (main_board.color_on[sq] == BLACK) c = (char)(c + 32);
            }
            printf(" %c |", c);
        }
        printf("\n  +---+---+---+---+---+---+---+---+\n");
    }
    char fen[128];
    board_to_fen(&main_board, fen);
    printf("    a   b   c   d   e   f   g   h\n\nFen: %s\nKey: %016llx\nEval: %d (side to move, %s)\n",
           fen, (unsigned long long)main_board.hash, nnue_evaluate_board(&main_board),
           nnue_loaded && nnue_enabled ? nnue_tier : "handcrafted");
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    board_init_all();
    search_init();
    search_set_threads(1);
    tt_alloc(hash_mb);
    nnue_load_embedded();
    parse_fen(&main_board, STARTPOS_FEN);

    if (argc > 1 && !strcmp(argv[1], "bench")) {
        run_bench(argc > 2 ? atoi(argv[2]) : 0);
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "perft")) {
        uint64_t n = perft(&main_board, atoi(argv[2]));
        printf("Perft(%d) = %llu\n", atoi(argv[2]), (unsigned long long)n);
        return 0;
    }

    char line[16384];
    while (fgets(line, sizeof line, stdin)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        char *cmd = line;
        while (*cmd == ' ' || *cmd == '\t') cmd++;

        if (!strcmp(cmd, "uci")) print_options();
        else if (!strcmp(cmd, "isready")) { printf("readyok\n"); fflush(stdout); }
        else if (!strcmp(cmd, "stop")) wait_search(true);
        else if (!strcmp(cmd, "ponderhit")) search_ponderhit();
        else if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) break;
        else if (!strncmp(cmd, "go", 2) && (cmd[2] == 0 || cmd[2] == ' ')) parse_go(cmd + 2);
        else {
            /* Anything else touches engine state: let the search finish first. */
            wait_search(true);
            if (!strcmp(cmd, "ucinewgame")) {
                tt_clear();
                search_clear();
                parse_fen(&main_board, STARTPOS_FEN);
                game_hist_len = 0;
            }
            else if (!strncmp(cmd, "setoption", 9)) handle_setoption(cmd);
            else if (!strncmp(cmd, "position", 8)) parse_position(cmd + 8);
            else if (!strncmp(cmd, "perft", 5)) perft_divide(&main_board, atoi(cmd + 5));
            else if (!strcmp(cmd, "d")) print_board();
            else if (!strcmp(cmd, "bench")) run_bench(0);
        }
    }

    wait_search(true);
    if (search_thread_valid) {
        pthread_mutex_lock(&job_mutex);
        uci_exit = true;
        pthread_cond_broadcast(&job_cond);
        pthread_mutex_unlock(&job_mutex);
        pthread_join(search_thread, NULL);
    }
    return 0;
}
