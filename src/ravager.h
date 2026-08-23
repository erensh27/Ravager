/*
 * ravager.h — Ravager 2.0 — shared types & constants
 * HCE + NNUE evaluation, Syzygy tablebases.
 */

#ifndef RAVAGER_H
#define RAVAGER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>

#define ENGINE_NAME    "Ravager"
#define ENGINE_VERSION "2"
#define ENGINE_AUTHOR  "Ravager Dev"

/* Colours */
#define WHITE 0
#define BLACK 1

/* Piece types */
#define PAWN   0
#define KNIGHT 1
#define BISHOP 2
#define ROOK   3
#define QUEEN  4
#define KING   5
#define NO_PIECE 6
#define PIECE_NB 6

/* Squares: a1 = 0 .. h8 = 63 */
#define A1 0
#define B1 1
#define C1 2
#define D1 3
#define E1 4
#define F1 5
#define G1 6
#define H1 7
#define D4 27
#define E4 28
#define D5 35
#define E5 36
#define A8 56
#define C8 58
#define D8 59
#define E8 60
#define F8 61
#define G8 62
#define H8 63
#define NO_SQUARE 64

/* Castling rights bits */
#define CASTLE_WK 1
#define CASTLE_WQ 2
#define CASTLE_BK 4
#define CASTLE_BQ 8

/* Move flags */
#define FLAG_NORMAL  0
#define FLAG_EP      1
#define FLAG_CASTLE  2
#define FLAG_PROMO   3

#define PROMO_KNIGHT 0
#define PROMO_BISHOP 1
#define PROMO_ROOK   2
#define PROMO_QUEEN  3

/* Tapered eval phases */
#define MG 0
#define EG 1
#define TOTAL_PHASE 24

/* Search limits */
#define MAX_PLY        128
#define MAX_MOVES      256
#define INFINITY_SCORE 32767
#define MATE_SCORE     32000
#define MATE_BOUND     31000
#define NO_MOVE        ((uint16_t)0)

/* TT bound types */
#define BOUND_EXACT 0
#define BOUND_LOWER 1
#define BOUND_UPPER 2

#define DEFAULT_TT_MB 256

typedef uint64_t Bitboard;
typedef uint16_t Move;

/* ---- Move encoding: from | to<<6 | promo<<12 | flags<<14 ---- */
static inline Move encode_move(int from, int to, int flags, int promo) {
    return (Move)((from) | (to << 6) | (promo << 12) | (flags << 14));
}
static inline int move_from(Move m)  { return m & 0x3F; }
static inline int move_to(Move m)    { return (m >> 6) & 0x3F; }
static inline int move_promo(Move m) { return (m >> 12) & 0x3; }
static inline int move_flags(Move m) { return (m >> 14) & 0x3; }
static inline bool move_is_ep(Move m)    { return move_flags(m) == FLAG_EP; }
static inline bool move_is_castle(Move m){ return move_flags(m) == FLAG_CASTLE; }
static inline bool move_is_promo(Move m) { return move_flags(m) == FLAG_PROMO; }

/* ---- Tapered score pair ---- */
typedef struct { int16_t mg, eg; } Score;
#define S(mg_val, eg_val) ((Score){(mg_val), (eg_val)})

static inline Score score_add(Score a, Score b) { return S(a.mg+b.mg, a.eg+b.eg); }
static inline Score score_sub(Score a, Score b) { return S(a.mg-b.mg, a.eg-b.eg); }

/* Board state for make/unmake */
typedef struct {
    uint64_t zobrist_key;
    uint8_t  castling_rights;
    uint8_t  ep_square;
    uint8_t  half_move_clock;
    uint8_t  captured_piece;   /* NO_PIECE = 6 */
    Move     move;
} UndoInfo;

typedef struct {
    Bitboard pieces[2][6];
    Bitboard occupancy[2];
    Bitboard occ_all;
    uint8_t  piece_on[64];     /* color*6 + type, 12 = empty */
    uint8_t  color_on[64];
    int32_t  psqt[2][2];       /* incremental material+PST: [color][MG|EG] */
    int16_t  game_phase;       /* incremental game phase (max 24) */
    int      side;
    uint8_t  castling_rights;
    uint8_t  ep_square;
    uint8_t  half_move_clock;
    int      full_move_number;
    uint64_t hash;
    int      ply;              /* search ply */
    int      game_ply;         /* plies since root of game */
    UndoInfo undo_stack[MAX_PLY * 2 + 512];
} Board;

#define EMPTY_SQUARE 12

typedef struct {
    Move moves[MAX_MOVES];
    int  scores[MAX_MOVES];
    int  count;
} MoveList;

/* Move gen */
void generate_moves(Board *b, MoveList *ml);      /* pseudo-legal; filter after make */
void generate_evasions(Board *b, MoveList *ml);   /* fully legal; check evasions only */
void generate_captures(Board *b, MoveList *ml);

/* Board */
bool parse_fen(Board *b, const char *fen);
void make_move(Board *b, Move m);
void unmake_move(Board *b, Move m);
void make_null_move(Board *b);
void unmake_null_move(Board *b);
bool is_square_attacked(const Board *b, int sq, int attacker_side);
bool is_in_check(const Board *b, int side);
Bitboard all_attackers_to(const Board *b, int sq, Bitboard occ);
int  see_move(Board *b, Move m);
int  evaluate(Board *b);
void board_init_all(void);   /* init every table */
void refresh_psqt_tables(void);
void board_refresh_psqt(Board *b);

extern const char *STARTPOS_FEN;
const char *move_to_str(Move m);
Move parse_move(const Board *b, const char *s);

/* UCI-exposed knobs */
extern int move_overhead_ms;

#endif /* RAVAGER_H */
