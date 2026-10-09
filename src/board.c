/* board.c — position representation, zobrist hashing, make/unmake, FEN */

#include "bitboard.h"
#include "params.h"

static uint64_t zobrist_piece[2][6][64];
static uint64_t zobrist_ep[8];
static uint64_t zobrist_castling[16];
static uint64_t zobrist_side;

bool uci_chess960 = false;

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

static uint64_t castle_key(const Board *b, int rights) {
    uint64_t key = zobrist_castling[rights];
    const int orthodox[4] = {H1,A1,H8,A8};
    for (int i=0;i<4;i++) if ((rights & (1<<i)) && b->castle_rook[i] != orthodox[i])
        key ^= zobrist_piece[i/2][ROOK][b->castle_rook[i]] ^ zobrist_piece[i/2][ROOK][orthodox[i]];
    return key;
}

static void compute_hash(Board *b) {
    b->hash = 0;
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < 6; p++) {
            Bitboard bb = b->pieces[c][p];
            while (bb) b->hash ^= zobrist_piece[c][p][lsb_pop(&bb)];
        }
    if (b->ep_square != NO_SQUARE) b->hash ^= zobrist_ep[file_of(b->ep_square)];
    b->hash ^= castle_key(b,b->castling_rights);
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
    for (int i=0;i<4;i++) b->castle_rook[i]=NO_SQUARE;
    memset(b->castle_mask,15,64);
    while (*p && *p != ' ') {
        char ch=*p++;
        if (ch=='-') continue;
        int c=(ch>='a' && ch<='z'), base=56*c;
        int k=b->pieces[c][KING]?lsb(b->pieces[c][KING]):base+4;
        int rf=-1;
        if (ch=='K'||ch=='k'||ch=='Q'||ch=='q') {
            bool ks=(ch=='K'||ch=='k');
            for(int file=0;file<8;file++) if(b->pieces[c][ROOK] & (1ULL<<(base+file))) {
                if (ks && file>file_of(k)) rf=base+file;
                if (!ks && file<file_of(k) && rf<0) rf=base+file;
            }
        } else if ((ch>='A'&&ch<='H')||(ch>='a'&&ch<='h')) rf=base+(c?ch-'a':ch-'A');
        if(rf<0 || !(b->pieces[c][ROOK] & (1ULL<<rf))) continue;
        int i=2*c+(rf<k);
        b->castle_rook[i]=(uint8_t)rf;
        b->castling_rights |= 1<<i;
        b->castle_mask[rf] &= ~(1<<i);
        b->castle_mask[k] &= ~(3<<(2*c));
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
    return !move_is_castle(m) && ((b->occ_all & (1ULL << move_to(m))) || move_is_ep(m));
}

static void castle_relocate(Board *b, int side, int from, int to, bool undo) {
    int i=2*side+(file_of(to)==2);
    int rf=b->castle_rook[i], rt=8*rank_of(to)+(file_of(to)==6?5:3);
    int kfrom=undo?to:from, kto=undo?from:to;
    int rfrom=undo?rt:rf, rto=undo?rf:rt;
    remove_piece(b,side,KING,kfrom);
    remove_piece(b,side,ROOK,rfrom);
    put_piece(b,side,KING,kto);
    put_piece(b,side,ROOK,rto);
    if(!undo) b->hash ^= zobrist_piece[side][KING][from] ^ zobrist_piece[side][KING][to]
                      ^ zobrist_piece[side][ROOK][rf] ^ zobrist_piece[side][ROOK][rt];
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

    if(flags==FLAG_CASTLE) castle_relocate(b,side,from,to,false);
    else {
        b->hash ^= zobrist_piece[side][piece][from] ^ zobrist_piece[side][placed][to];
        remove_piece(b, side, piece, from);
        put_piece(b, side, placed, to);
    }

    uint8_t old_cr = b->castling_rights;
    b->castling_rights &= b->castle_mask[from];
    b->castling_rights &= b->castle_mask[to];
    b->hash ^= castle_key(b,old_cr) ^ castle_key(b,b->castling_rights);

    /* Only set ep square if an enemy pawn could actually capture there
     * (keeps hash keys cleaner — conditional ep) */
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

    if (flags == FLAG_CASTLE) castle_relocate(b,side,from,to,true);
    else {
        remove_piece(b, side, placed, to);
        put_piece(b, side, piece, from);
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
    static _Thread_local char buf[8];
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
        for(int i=2*b->side;i<2*b->side+2;i++) if(b->castling_rights & (1<<i)) {
            int dest=8*b->side*7+(i%2?2:6);
            if(to==(uci_chess960?b->castle_rook[i]:dest)) {to=dest;flags=FLAG_CASTLE;break;}
        }
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

const char *move_to_uci(const Board *b, Move m) {
    if(uci_chess960 && move_is_castle(m)) {
        int i=2*b->side+(file_of(move_to(m))==2);
        m=encode_move(move_from(m),b->castle_rook[i],FLAG_NORMAL,0);
    }
    return move_to_str(m);
}

bool board_start960(Board *b, int index) {
    if(index<0 || index>959) return false;
    char row[9]="........", fen[128]; int n=index;
    row[2*(n%4)+1]='B'; n/=4; row[2*(n%4)]='B'; n/=4;
    int freefiles[8],count=0;
    for(int i=0;i<8;i++) if(row[i]=='.') freefiles[count++]=i;
    row[freefiles[n%6]]='Q'; n/=6;
    count=0;for(int i=0;i<8;i++) if(row[i]=='.') freefiles[count++]=i;
    int pair=0;
    for(int a=0;a<4;a++) for(int d=a+1;d<5;d++) if(pair++==n) {row[freefiles[a]]='N';row[freefiles[d]]='N';}
    count=0;for(int i=0;i<8;i++) if(row[i]=='.') freefiles[count++]=i;
    row[freefiles[0]]='R';row[freefiles[1]]='K';row[freefiles[2]]='R';
    char black[9];for(int i=0;i<8;i++) black[i]=row[i]+32;black[8]=0;
    snprintf(fen,sizeof fen,"%s/pppppppp/8/8/8/8/PPPPPPPP/%s w KQkq - 0 1",black,row);
    return parse_fen(b,fen);
}
