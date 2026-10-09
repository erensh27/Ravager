/*
 * tbconfig.h — Pyrrhic configuration wired to Ravager's bitboard layer.
 *
 * Pyrrhic (Syzygy probing, up to 7 men) by basil, Jon Dart and Andrew Grant.
 * MIT licensed — see tb/LICENSE.
 *
 * Colour note: Pyrrhic uses BLACK=0 / WHITE=1; Ravager uses WHITE=0 /
 * BLACK=1, so every colour argument is inverted (^1) on the way in.
 */

#pragma once

#include "../bitboard.h"

#define PYRRHIC_POPCOUNT(x)              (popcount(x))
#define PYRRHIC_LSB(x)                   (lsb(x))
#define PYRRHIC_POPLSB(x)                (lsb_pop(x))

#define PYRRHIC_PAWN_ATTACKS(sq, c)      (pawn_attack_table[(c) ^ 1][sq])
#define PYRRHIC_KNIGHT_ATTACKS(sq)       (knight_attack_table[sq])
#define PYRRHIC_BISHOP_ATTACKS(sq, occ)  (bishop_attacks(sq, occ))
#define PYRRHIC_ROOK_ATTACKS(sq, occ)    (rook_attacks(sq, occ))
#define PYRRHIC_QUEEN_ATTACKS(sq, occ)   (queen_attacks(sq, occ))
#define PYRRHIC_KING_ATTACKS(sq)         (king_attack_table[sq])
