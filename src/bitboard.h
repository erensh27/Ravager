/* bitboard.h — bit twiddling, attack tables (BMI2/PEXT with magic fallback) */
#ifndef BITBOARD_H
#define BITBOARD_H

#include "ravager.h"

static inline int lsb(Bitboard b)            { return __builtin_ctzll(b); }
static inline int lsb_pop(Bitboard *b)       { int s = __builtin_ctzll(*b); *b &= *b - 1; return s; }
static inline int msb(Bitboard b)            { return 63 - __builtin_clzll(b); }
static inline int popcount(Bitboard b)       { return __builtin_popcountll(b); }
static inline int rank_of(int sq)            { return sq >> 3; }
static inline int file_of(int sq)            { return sq & 7; }
static inline int sq_of(int r, int f)        { return (r << 3) | f; }
static inline int sq_flip(int sq)            { return sq ^ 56; }
static inline int relative_rank(int c, int sq){ return c == WHITE ? rank_of(sq) : 7 - rank_of(sq); }

#define FILE_A  0x0101010101010101ULL
#define FILE_B  0x0202020202020202ULL
#define FILE_C  0x0404040404040404ULL
#define FILE_D  0x0808080808080808ULL
#define FILE_E  0x1010101010101010ULL
#define FILE_F  0x2020202020202020ULL
#define FILE_G  0x4040404040404040ULL
#define FILE_H  0x8080808080808080ULL
#define RANK_1  0x00000000000000FFULL
#define RANK_2  0x000000000000FF00ULL
#define RANK_3  0x0000000000FF0000ULL
#define RANK_4  0x00000000FF000000ULL
#define RANK_5  0x000000FF000000ULL
#define RANK_6  0x0000FF0000000000ULL
#define RANK_7  0x00FF000000000000ULL
#define RANK_8  0xFF00000000000000ULL
#define NOT_FILE_A (~FILE_A)
#define NOT_FILE_H (~FILE_H)
#define CENTER_FILES (FILE_C|FILE_D|FILE_E|FILE_F)
#define CENTER_SQS (0x0000001818000000ULL)

extern Bitboard knight_attack_table[64];
extern Bitboard king_attack_table[64];
extern Bitboard pawn_attack_table[2][64];
extern Bitboard passed_pawn_mask[2][64];
extern Bitboard isolated_mask[64];
extern Bitboard forward_file_mask[2][64];
extern Bitboard between_table[64][64];
extern Bitboard ray_table[64][64];
extern Bitboard file_bitboard[8];
extern Bitboard adjacent_files[8];

Bitboard rook_attacks(int sq, Bitboard occ);
Bitboard bishop_attacks(int sq, Bitboard occ);
Bitboard queen_attacks(int sq, Bitboard occ);

void init_bitboards(void);

#endif
