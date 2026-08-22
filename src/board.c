/* board.c — position representation, zobrist hashing, make/unmake, FEN */

#include "bitboard.h"
#include "params.h"

static uint64_t zobrist_piece[2][6][64];
static uint64_t zobrist_ep[8];
static uint64_t zobrist_castling[16];
static uint64_t zobrist_side;

const char *STARTPOS_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

/* Combined material + PST tables used by the incremental accumulators.
 * Black mirrors the square (sq ^ 56) at lookup time, same as the eval. */
static int psqt_mg_table[6][64];
static int psqt_eg_table[6][64];

void refresh_psqt_tables(void) {
    for (int p = 0; p < 6; p++)
        for (int sq = 0; sq < 64; sq++) {
            psqt_mg_table[p][sq] = PST[p][MG][sq];
            psqt_eg_table[p][sq] = PST[p][EG][sq];
        }
}

static void init_psqt_tables(void) { refresh_psqt_tables(); }

/* Recompute one board's incremental accumulators from piece placement
 * (used by the tuner after mutating PST). */
void board_refresh_psqt(Board *b) {
    b->psqt[WHITE][MG] = b->psqt[WHITE][EG] = 0;
    b->psqt[BLACK][MG] = b->psqt[BLACK][EG] = 0;
    b->game_phase = 0;
    for (int sq = 0; sq < 64; sq++) {
        if (b->piece_on[sq] == EMPTY_SQUARE) continue;
        int c = b->color_on[sq], p = b->piece_on[sq] % 6;
        int tsq = (c == WHITE) ? sq : sq_flip(sq);
        b->psqt[c][MG] += psqt_mg_table[p][tsq];
        b->psqt[c][EG] += psqt_eg_table[p][tsq];
        b->game_phase = (int16_t)(b->game_phase + PHASE_VALUES[p]);
    }
}

static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *state = x;
    return x;
}

static void init_zobrist(void) {
    uint64_t seed = 0x9F1B2C3D4E5A6B7CULL;
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < 6; p++)
            for (int sq = 0; sq < 64; sq++)
                zobrist_piece[c][p][sq] = xorshift64(&seed);
    for (int f = 0; f < 8; f++) zobrist_ep[f] = xorshift64(&seed);
    for (int cr = 0; cr < 16; cr++) zobrist_castling[cr] = xorshift64(&seed);
    zobrist_side = xorshift64(&seed);
}

static const uint8_t castling_rights_mask[64] = {
    13,15,15,15,12,15,15,14,
    15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15,
    15,15,15,15,15,15,15,15,
     7,15,15,15, 3,15,15,11,
};

static inline void put_piece(Board *b, int color, int piece, int sq) {
    Bitboard bit = 1ULL << sq;
    b->pieces[color][piece] |= bit;
    b->occupancy[color]     |= bit;
    b->occ_all              |= bit;
    b->piece_on[sq] = (uint8_t)(color * 6 + piece);
    b->color_on[sq] = (uint8_t)color;
    int tsq = (color == WHITE) ? sq : sq_flip(sq);
    b->psqt[color][MG] += psqt_mg_table[piece][tsq];
    b->psqt[color][EG] += psqt_eg_table[piece][tsq];
    b->game_phase      = (int16_t)(b->game_phase + PHASE_VALUES[piece]);
}

static inline void remove_piece(Board *b, int color, int piece, int sq) {
    Bitboard bit = 1ULL << sq;
    b->pieces[color][piece] &= ~bit;
    b->occupancy[color]     &= ~bit;
    b->occ_all              &= ~bit;
    b->piece_on[sq] = EMPTY_SQUARE;
    b->color_on[sq] = 0;
    int tsq = (color == WHITE) ? sq : sq_flip(sq);
    b->psqt[color][MG] -= psqt_mg_table[piece][tsq];
    b->psqt[color][EG] -= psqt_eg_table[piece][tsq];
    b->game_phase      = (int16_t)(b->game_phase - PHASE_VALUES[piece]);
}

static inline void move_piece_bb(Board *b, int color, int piece, int from, int to) {
    Bitboard bits = (1ULL << from) | (1ULL << to);
    b->pieces[color][piece] ^= bits;
    b->occupancy[color]     ^= bits;
    b->occ_all              ^= bits;
    b->piece_on[from] = EMPTY_SQUARE;
    b->piece_on[to]   = (uint8_t)(color * 6 + piece);
    b->color_on[to]   = (uint8_t)color;
    b->color_on[from] = 0;
    int tfrom = (color == WHITE) ? from : sq_flip(from);
    int tto   = (color == WHITE) ? to   : sq_flip(to);
    b->psqt[color][MG] += psqt_mg_table[piece][tto] - psqt_mg_table[piece][tfrom];
    b->psqt[color][EG] += psqt_eg_table[piece][tto] - psqt_eg_table[piece][tfrom];
}

static void clear_board(Board *b) {
    memset(b, 0, sizeof(*b));
    for (int sq = 0; sq < 64; sq++) b->piece_on[sq] = EMPTY_SQUARE;
    b->ep_square = NO_SQUARE;
    b->full_move_number = 1;
}

static int char_to_piece(char c) {
    switch (c) {
        case 'p': case 'P': return PAWN;
        case 'n': case 'N': return KNIGHT;
        case 'b': case 'B': return BISHOP;
        case 'r': case 'R': return ROOK;
        case 'q': case 'Q': return QUEEN;
        case 'k': case 'K': return KING;
    }
    return NO_PIECE;
}

static void compute_hash(Board *b) {
    b->hash = 0;
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < 6; p++) {
            Bitboard bb = b->pieces[c][p];
            while (bb) b->hash ^= zobrist_piece[c][p][lsb_pop(&bb)];
        }
    if (b->ep_square != NO_SQUARE) b->hash ^= zobrist_ep[file_of(b->ep_square)];
    b->hash ^= zobrist_castling[b->castling_rights];
    if (b->side == BLACK) b->hash ^= zobrist_side;
}

bool parse_fen(Board *b, const char *fen) {
    clear_board(b);
    const char *p = fen;

    int sq = 56;
    while (*p && *p != ' ') {
        if (*p == '/') { sq -= 16; p++; }
        else if (*p >= '1' && *p <= '8') sq += (*p++ - '0');
        else {
            int color = (*p >= 'a' && *p <= 'z') ? BLACK : WHITE;
            int piece = char_to_piece(*p++);
            if (piece != NO_PIECE) put_piece(b, color, piece, sq++);
        }
    }
    if (*p == ' ') p++;

    b->side = (*p == 'b') ? BLACK : WHITE;
    while (*p && *p != ' ') p++;
    if (*p == ' ') p++;

    b->castling_rights = 0;
    while (*p && *p != ' ') {
        if (*p == 'K') b->castling_rights |= CASTLE_WK;
        else if (*p == 'Q') b->castling_rights |= CASTLE_WQ;
        else if (*p == 'k') b->castling_rights |= CASTLE_BK;
        else if (*p == 'q') b->castling_rights |= CASTLE_BQ;
        p++;
    }
    if (*p == ' ') p++;

    b->ep_square = NO_SQUARE;
    if (*p != '-') {
        int f = p[0] - 'a', r = p[1] - '1';
        if (f >= 0 && f < 8 && r >= 0 && r < 8) b->ep_square = (uint8_t)(r * 8 + f);
    }
    while (*p && *p != ' ') p++;
    if (*p == ' ') p++;

    b->half_move_clock = 0;
    if (*p && *p != ' ') {
        b->half_move_clock = (uint8_t)atoi(p);
        while (*p && *p != ' ') p++;
        if (*p == ' ') p++;
    }
    b->full_move_number = 1;
    if (*p) b->full_move_number = atoi(p);

    compute_hash(b);
    return true;
}

bool is_square_attacked(const Board *b, int sq, int attacker_side) {
    Bitboard occ = b->occ_all;
    if (pawn_attack_table[attacker_side^1][sq] & b->pieces[attacker_side][PAWN])   return true;
    if (knight_attack_table[sq] & b->pieces[attacker_side][KNIGHT])                return true;
    if (king_attack_table[sq]   & b->pieces[attacker_side][KING])                  return true;
    if (rook_attacks(sq, occ)   & (b->pieces[attacker_side][ROOK]   | b->pieces[attacker_side][QUEEN])) return true;
    if (bishop_attacks(sq, occ) & (b->pieces[attacker_side][BISHOP] | b->pieces[attacker_side][QUEEN])) return true;
    return false;
}

bool is_in_check(const Board *b, int side) {
    if (!b->pieces[side][KING]) return false;
    return is_square_attacked(b, lsb(b->pieces[side][KING]), side ^ 1);
}

static inline bool is_king_move_legal(Board *b, int to, int opp, int king_sq) {
    b->occ_all ^= (1ULL << king_sq);
    bool attacked = is_square_attacked(b, to, opp);
    b->occ_all ^= (1ULL << king_sq);
    return !attacked;
}

Bitboard all_attackers_to(const Board *b, int sq, Bitboard occ) {
    return (pawn_attack_table[BLACK][sq] & b->pieces[WHITE][PAWN]) |
           (pawn_attack_table[WHITE][sq] & b->pieces[BLACK][PAWN]) |
           (knight_attack_table[sq] & (b->pieces[WHITE][KNIGHT] | b->pieces[BLACK][KNIGHT])) |
           (king_attack_table[sq]   & (b->pieces[WHITE][KING]   | b->pieces[BLACK][KING]))   |
           (rook_attacks(sq, occ)   & (b->pieces[WHITE][ROOK]   | b->pieces[BLACK][ROOK]   |
                                       b->pieces[WHITE][QUEEN]  | b->pieces[BLACK][QUEEN])) |
           (bishop_attacks(sq, occ) & (b->pieces[WHITE][BISHOP] | b->pieces[BLACK][BISHOP] |
                                       b->pieces[WHITE][QUEEN]  | b->pieces[BLACK][QUEEN]));
}

static inline bool move_is_capture_bb(const Board *b, Move m) {
    return (b->occ_all & (1ULL << move_to(m))) || move_is_ep(m);
}

static void move_rook_castle(Board *b, int side, int to) {
    int rook_from, rook_to;
    if (to == G1)      { rook_from = H1; rook_to = F1; }
    else if (to == C1) { rook_from = A1; rook_to = D1; }
    else if (to == G8) { rook_from = H8; rook_to = F8; }
    else               { rook_from = A8; rook_to = D8; }
    b->hash ^= zobrist_piece[side][ROOK][rook_from] ^ zobrist_piece[side][ROOK][rook_to];
    move_piece_bb(b, side, ROOK, rook_from, rook_to);
}

void make_move(Board *b, Move m) {
    int from = move_from(m), to = move_to(m), flags = move_flags(m);
    int side = b->side, opp = side ^ 1;

    UndoInfo *u = &b->undo_stack[b->ply];
    u->move = m;
    u->zobrist_key = b->hash;
    u->castling_rights = b->castling_rights;
    u->ep_square = b->ep_square;
    u->half_move_clock = b->half_move_clock;
    u->captured_piece = NO_PIECE;

    int piece = b->piece_on[from] % 6;
    int placed = piece;

    if (b->ep_square != NO_SQUARE) {
        b->hash ^= zobrist_ep[file_of(b->ep_square)];
        b->ep_square = NO_SQUARE;
    }

    int cap_sq = to;
    if (flags == FLAG_EP) cap_sq = (side == WHITE) ? to - 8 : to + 8;

    if (flags == FLAG_EP || ((b->occ_all & (1ULL << cap_sq)) && (b->occupancy[opp] & (1ULL << cap_sq)))) {
        int captured = (flags == FLAG_EP) ? PAWN : (b->piece_on[cap_sq] % 6);
        u->captured_piece = (uint8_t)captured;
        b->hash ^= zobrist_piece[opp][captured][cap_sq];
        remove_piece(b, opp, captured, cap_sq);
    }

    if (flags == FLAG_PROMO) {
        static const int promo_piece[] = {KNIGHT, BISHOP, ROOK, QUEEN};
        placed = promo_piece[move_promo(m)];
    }

    b->hash ^= zobrist_piece[side][piece][from] ^ zobrist_piece[side][placed][to];
    remove_piece(b, side, piece, from);
    put_piece(b, side, placed, to);

    if (flags == FLAG_CASTLE) move_rook_castle(b, side, to);

    uint8_t old_cr = b->castling_rights;
    b->castling_rights &= castling_rights_mask[from];
    b->castling_rights &= castling_rights_mask[to];
    b->hash ^= zobrist_castling[old_cr] ^ zobrist_castling[b->castling_rights];

    /* Only set ep square if an enemy pawn could actually capture there
     * (keeps hash keys cleaner — Stockfish-style conditional ep) */
    if (piece == PAWN && abs(to - from) == 16) {
        int ep = (side == WHITE) ? from + 8 : from - 8;
        if (pawn_attack_table[side][ep] & b->pieces[opp][PAWN]) {
            b->ep_square = (uint8_t)ep;
            b->hash ^= zobrist_ep[file_of(ep)];
        }
    }

    if (piece == PAWN || u->captured_piece != NO_PIECE) b->half_move_clock = 0;
    else b->half_move_clock++;

    b->hash ^= zobrist_side;
    b->side = opp;
    b->ply++;
    b->game_ply++;
    if (b->side == WHITE) b->full_move_number++;
}

void unmake_move(Board *b, Move m) {
    b->ply--;
    b->game_ply--;
    if (b->side == WHITE) b->full_move_number--;
    b->side ^= 1;
    int side = b->side, opp = side ^ 1;

    UndoInfo *u = &b->undo_stack[b->ply];
    b->hash = u->zobrist_key;
    b->castling_rights = u->castling_rights;
    b->ep_square = u->ep_square;
    b->half_move_clock = u->half_move_clock;

    int from = move_from(m), to = move_to(m), flags = move_flags(m);
    int placed = b->piece_on[to] % 6;
    int piece = placed;
    if (flags == FLAG_PROMO) piece = PAWN;

    remove_piece(b, side, placed, to);
    put_piece(b, side, piece, from);

    if (flags == FLAG_CASTLE) {
        int rook_from, rook_to;
        if (to == G1)      { rook_from = H1; rook_to = F1; }
        else if (to == C1) { rook_from = A1; rook_to = D1; }
        else if (to == G8) { rook_from = H8; rook_to = F8; }
        else               { rook_from = A8; rook_to = D8; }
        move_piece_bb(b, side, ROOK, rook_to, rook_from);
    }

    if (u->captured_piece != NO_PIECE) {
        int cap_sq = (flags == FLAG_EP) ? ((side == WHITE) ? to - 8 : to + 8) : to;
        put_piece(b, opp, u->captured_piece, cap_sq);
    }
}

void make_null_move(Board *b) {
    UndoInfo *u = &b->undo_stack[b->ply];
    u->zobrist_key = b->hash;
    u->ep_square = b->ep_square;
    u->castling_rights = b->castling_rights;
    u->half_move_clock = b->half_move_clock;
    u->captured_piece = NO_PIECE;
    u->move = NO_MOVE;

    if (b->ep_square != NO_SQUARE) {
        b->hash ^= zobrist_ep[file_of(b->ep_square)];
        b->ep_square = NO_SQUARE;
    }
    b->hash ^= zobrist_side;
    b->side ^= 1;
    b->ply++;
    b->game_ply++;
}

void unmake_null_move(Board *b) {
    b->ply--;
    b->game_ply--;
    b->side ^= 1;
    UndoInfo *u = &b->undo_stack[b->ply];
    b->hash = u->zobrist_key;
    b->ep_square = u->ep_square;
    b->castling_rights = u->castling_rights;
    b->half_move_clock = u->half_move_clock;
}

const char *move_to_str(Move m) {
    static char buf[8];
    int f = move_from(m), t = move_to(m);
    buf[0] = 'a' + file_of(f);
    buf[1] = '1' + rank_of(f);
    buf[2] = 'a' + file_of(t);
    buf[3] = '1' + rank_of(t);
    if (move_is_promo(m)) { buf[4] = "nbrq"[move_promo(m)]; buf[5] = 0; }
    else buf[4] = 0;
    return buf;
}

Move parse_move(const Board *b, const char *s) {
    if (!s || strlen(s) < 4) return NO_MOVE;
    int from = sq_of(s[1]-'1', s[0]-'a');
    int to   = sq_of(s[3]-'1', s[2]-'a');
    if (from < 0 || from > 63 || to < 0 || to > 63) return NO_MOVE;
    int promo = 0, flags = FLAG_NORMAL;
    switch (s[4]) {
        case 'n': case 'N': promo = PROMO_KNIGHT; flags = FLAG_PROMO; break;
        case 'b': case 'B': promo = PROMO_BISHOP; flags = FLAG_PROMO; break;
        case 'r': case 'R': promo = PROMO_ROOK;   flags = FLAG_PROMO; break;
        case 'q': case 'Q': promo = PROMO_QUEEN;  flags = FLAG_PROMO; break;
        default: break;
    }
    if (b->piece_on[from] != EMPTY_SQUARE && b->piece_on[from] % 6 == KING) {
        if ((from == E1 && to == G1) || (from == E8 && to == G8) ||
            (from == E1 && to == C1) || (from == E8 && to == C8)) flags = FLAG_CASTLE;
    }
    if (b->piece_on[from] != EMPTY_SQUARE && b->piece_on[from] % 6 == PAWN &&
        b->ep_square != NO_SQUARE && to == b->ep_square) flags = FLAG_EP;
    return encode_move(from, to, flags, promo);
}

void board_init_all(void) {
    init_psqt_tables();
    init_zobrist();
    init_bitboards();
}
