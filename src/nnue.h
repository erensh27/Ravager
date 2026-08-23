/* nnue.h — efficiently updatable neural network evaluation.
 *
 * Two network formats are supported natively (detected at load time):
 *
 *   1. "Ravager" format (self-trained nets; see below)
 *      Architecture: (768 -> H)x2 -> 1, plain piece-placement features,
 *      ClippedReLU activations, single output bucket.
 *      Binary layout (little-endian):
 *        [4] magic 0x4E4E5545 ("NNUE"), [4] version 1
 *        [768*H] int16 ft-weights (x QA)   [H] int16 ft-biases (x QA)
 *        [2*H]   int16 out-weights (x QB)  [4] int32 out-bias (x QA*QB)
 *        score_cp = (sum(clamp(acc,0,QA)*w) + bias) / (QA*QB),  QA=QB=64
 *
 *   2. "Leorik" format (github.com/lithander/Leorik releases, filename
 *      pattern "<H>HL-S-<K>io<O>-*.nnue")
 *      Architecture: (768*K -> H)x2 -> O with king-zone input buckets and
 *      material output buckets, SCReLU activation.
 *      Feature index (per perspective): bucket*768 + own/enemy block (384)
 *      + pieceType*64 + square, where the square is mirrored horizontally
 *      when the perspective king stands on files e-h.
 *      score_cp = ((sum(clamp(acc,0,255)^2*w)) / 255 + bias) * 400 / (255*64)
 *
 * Both share the incremental dual-perspective accumulator: a small stack
 * anchored at the search root, updated by feature add/sub on every move.
 * When a king crosses an input-bucket boundary (Leorik nets only) the
 * affected perspective is marked invalid and lazily rebuilt at eval time.
 */
#ifndef NNUE_H
#define NNUE_H

#include "ravager.h"

#define NN_INPUT      768
#define NN_HIDDEN_MAX 640
#define NN_OUTBUCK_MAX  8

enum { NET_NONE = 0, NET_RAVAGER = 1, NET_LEORIK = 2 };

#define NNUE_MAGIC   0x4E4E5545u
#define NNUE_VERSION 1
#define NNUE_QA 64          /* Ravager-format ft quantisation scale */
#define NNUE_QB 64          /* Ravager-format output quantisation scale */

/* Per-perspective accumulated first layer plus king-bucket state.
 * `kvalid[p] == 0` means vals[p] must be rebuilt before use. */
typedef struct {
    int16_t vals[2][NN_HIDDEN_MAX];
    uint8_t kbucket[2];   /* input bucket per perspective */
    uint8_t kmirror[2];   /* horizontal-mirror flag per perspective */
    uint8_t kvalid[2];
} Accumulator;

typedef struct {
    int      format;
    int      hidden;        /* layer-1 width                    */
    int      in_buckets;    /* king-zone input buckets          */
    int      out_buckets;   /* material output buckets          */
    bool     horiz_mirror;  /* leorik-style file mirroring      */
    int16_t *ft_w;          /* [in_buckets*768][hidden]         */
    int16_t *ft_b;          /* [hidden]                         */
    int16_t *out_w;         /* [out_buckets][2][hidden]         */
    int32_t *out_b;         /* [out_buckets], format-dependent scale */
    /* quantisation / scaling parameters */
    int qa, qb;
    int rav_out_bias;       /* NET_RAVAGER: single int32 bias   */
} Network;

extern Network nnue_net;
extern bool    nnue_loaded;    /* a network is resident             */
extern bool    nnue_enabled;   /* UCI "Use NNUE" toggle             */

/* Loaders. A Leorik-format file is recognised by its filename pattern or,
 * failing that, by its size; Ravager-format by its magic number. */
bool nnue_load_file(const char *path);
bool nnue_load_embedded(void);

/* Full accumulator refresh for an arbitrary board (tests, interactive). */
void nnue_refresh_accumulator(Accumulator *acc, const Board *b);

/* ---- Search-side API -------------------------------------------------- */

void nnue_prepare_search(const Board *b);   /* anchor root accumulator  */
void nnue_push_move(const Board *b, Move m);/* call right before make_move */
void nnue_push_null(const Board *b);        /* call right before null   */

/* Static evaluation used by the search: NNUE when loaded and enabled,
 * otherwise the handcrafted eval. Side-to-move POV, centipawns. */
int nnue_eval(const Board *b);

/* Interactive/full evaluation (recomputes accumulators from scratch). */
int nnue_evaluate_board(const Board *b);

#endif /* NNUE_H */
