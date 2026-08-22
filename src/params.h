/* params.h — all evaluation parameters as mutable globals.
 *
 * Everything the evaluation reads (and therefore everything the texel
 * tuner optimises) lives in these arrays in params.c. The tuner mutates
 * them in place and regenerates params.c when done.
 *
 * Phases: index 0 = middlegame (MG), 1 = endgame (EG). */
#ifndef PARAMS_H
#define PARAMS_H

#include "ravager.h"

/* Combined material + piece-square tables, white's point of view
 * (black mirrors with sq ^ 56 at accumulation time). */
extern int32_t PST[6][2][64];

/* Phase and material references (material folded into PST but kept
 * separately for endgame scaling logic). */
extern int32_t PHASE_VALUES[6];
extern int32_t MATERIAL_MG[6];
extern int32_t MATERIAL_EG[6];

/* Mobility buckets */
extern int32_t KNIGHT_MOB[2][9];
extern int32_t BISHOP_MOB[2][14];
extern int32_t ROOK_MOB[2][15];
extern int32_t QUEEN_MOB[2][28];

/* Pawn structure */
extern int32_t DOUBLED[2];
extern int32_t ISOLATED[2];
extern int32_t BACKWARD[2];
extern int32_t CONNECTED[2][8];

/* Passed pawns */
extern int32_t PASSED_BASE[2][8];
extern int32_t PASSED_FREE[2];
extern int32_t PASSED_DEFENDED[2];
extern int32_t PASSER_KD[2][8];     /* [own/enemy king][chebyshev distance] */

/* Piece terms */
extern int32_t BISHOP_PAIR[2];
extern int32_t ROOK_OPEN[2];
extern int32_t ROOK_SEMI[2];
extern int32_t ROOK_DOUBLED[2];
extern int32_t ROOK_BEHIND[2];
extern int32_t KNIGHT_OUTPOST[2];
extern int32_t BISHOP_OUTPOST[2];
extern int32_t QUEEN_EARLY[2];
extern int32_t BAD_BISHOP[2];       /* own pawns fixed on the bishop's colour */

/* King safety */
extern int32_t SHIELD_MISSING[2];
extern int32_t STORM[2][5];

/* Threats */
extern int32_t THREAT_PAWN_MINOR[2];
extern int32_t THREAT_PAWN_ROOKQ[2];
extern int32_t THREAT_Q_BY_MINOR[2];
extern int32_t THREAT_Q_BY_ROOK[2];

/* Space and tempo */
extern int32_t SPACE[2];
extern int32_t TEMPO[2];

#endif
