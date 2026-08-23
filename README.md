# Ravager 2

Ravager 1 was a pure handcrafted-evaluation (HCE) engine. **Ravager 2** keeps
that battle-tested search and HCE, and adds the three big upgrades:

| | Ravager 1 | Ravager 2 |
|---|---|---|
| Evaluation | HCE only | **NNUE** (768x2 nets, Leorik-format nets supported) with HCE fallback |
| Endgames | — | **Syzygy tablebases** (WDL in search, DTZ at root) via Pyrrhic |
| Eval tuning | hand-tuned params | **texel tuner v2**: EPD datasets, golden-section K, fixed snapshot bug |

The search (PVS + aspiration, TT, killers/countermoves, butterfly +
continuation history, LMR/LMP, RFP/razoring/futility, adaptive null move,
Probcut-lite, singular extensions, SEE-pruned qsearch) is unchanged from
Ravager 1 — NNUE slots in as a drop-in replacement for `evaluate()`, exactly
as described in `nnue_build.md`.

---

## BUILD

```bash
make                                   # native build (x86-64, BMI2/POPCNT)
make CFLAGS="-O3 -std=c11 -mbmi2 -mpopcnt"   # portable build
```

The default build **embeds the NNUE net into the executable** (~5 MB) via
incbin — `./ravager` is fully self-contained, no external files needed.
Alternatives:

```bash
make EVALFILE=path/to/net.nnue   # embed a different net
make EVALFILE=                   # HCE-only binary (~170 KB), no net
make net                         # re-download the bundled net if missing
```

## NNUE

Architecture support (`src/nnue.c`):

* **Ravager format** (self-trained nets): `(768 -> H)x2 -> 1`, plain
  piece-placement features, ClippedReLU, QA=QB=64. Header:
  `"NNUE", version 1` then int16 arrays `[768xH weights][H biases][2H out
  weights][int32 out bias]`.
* **Leorik format** (`<H>HL-S-<K>io<O>-*.nnue`, e.g. the bundled-compatible
  `nets/640HL-S-5io8-6116M-FRCv1.nnue` from Leorik's releases): king-zone
  input buckets (5), material output buckets (8), SCReLU activation,
  QA=255/QB=64/scale=400. Loaded natively — no conversion needed.

Both share one incremental dual-perspective accumulator anchored at the
search root; king crossing a bucket boundary invalidates only that
perspective, which is lazily rebuilt.

Verification tooling:

```bash
RAVAGER_EVALFILE=<net> ./ravager bench              # search with the net
RAVAGER_EVALFILE=<net> RAVAGER_NNUE_VERIFY=1 ./ravager bench   # accumulator self-check vs full rebuilds
python3 tools/verify_leorik.py nets/<net>.nnue <fen>...          # independent forward-pass reference
```

The shipped net was validated three ways: 3.1M incremental-vs-fresh
accumulator checks (0 mismatches), exact score match against an independent
Python implementation of Leorik's forward pass on startpos/Kiwipete/endgame
positions, and unchanged behaviour when disabled.

Training your own net: generate data with `./ravager datagen`, train with
[bullet](https://github.com/jw1912/bullet) (feature set `768`, hidden 256)
and export int16 arrays per the Ravager format above; or simply keep using
Leorik-release nets, which are drop-in compatible.

## SYZYGY TABLEBASES

Uses [Pyrrhic](https://github.com/AndyGrant/Pyrrhic) (vendored in `src/tb/`,
MIT). WDL tables are probed inside the search (scores on the mate scale,
blessed-loss/cursed-win handling per `Syzygy50MoveRule`); DTZ decides the
bestmove outright at root when the position is within range.

```
setoption name SyzygyPath value /path/to/syzygy/files
```

Multiple directories are separated by `:` (Linux) or `;` (Windows).
3-4-5-piece files (~940 MB total) fit this class of hardware fine; grab them
from `https://tablebase.lichess.ovh/tables/standard/3-4-5-wdl/` (+ `-dtz/`).
A smoke-test set lives in `syzygy/` (KRvK, KQvK, KPvK).

## TEXEL TUNER (v2)

`src/tuner.c` minimises E = mean((result − sigmoid(eval/K))²) over every
parameter in `params.c`: closed-form sweeps for PST entries, finite-difference
sweeps for all other groups, deterministic shuffle with a 10% holdout split,
and golden-section search for K each round (texel_tuning.md step 4).

```bash
make tuner
./tuner data_quiet.epd src/params_tuned.c 4        # 4 rounds
mv src/params_tuned.c src/params.c && make         # install + rebuild
./tuner mydata.txt src/params.c 4                  # ravager-datagen format also accepted
```

* Input: EPD lines `<FEN> c9 "1-0";` (Zurichess/lithess-big3 style — the
  classic `quiet-labeled.epd`, 725k quiet positions, ships as
  `data_quiet.epd`) or plain `"<FEN> <result>"` from `./ravager datagen`.
* In-check and |eval| > 2000cp positions are skipped automatically.
* Progress is logged to stderr; watch that HOLDOUT error tracks TRAIN error.
* Note for tuners: `piece_on[]` snapshots store bare piece types — earlier
  revisions corrupted evals through a colour/type encoding mixup (fixed).

Expected gain over the hand-tuned baseline: +100–200 elo (see
texel_tuning.md).

## TOOLS

```bash
./ravager bench                                  # fixed-depth benchmark
./ravager perft <depth>                          # movegen verification
./ravager datagen <games> <ms> <seed> <file>     # self-play training data
make tuner                                       # texel tuner binary
python3 tools/verify_leorik.py <net> <fens...>   # NNUE reference check
```

---

## UCI OPTIONS (complete list)

| Option | Type | Default | Description |
|---|---|---|---|
| `Hash` | spin | 256 | Transposition table size in MB (1–65536). |
| `MoveOverhead` | spin | 20 | Safety margin (ms) subtracted from engine time budgets. |
| `Ponder` | check | false | Accepted; ponder output is provided but pondered search is not implemented. |
| `Use NNUE` | check | true | Evaluate with a loaded NNUE net; `false` falls back to the handcrafted eval. |
| `EvalFile` | string | \<empty\> | Path to an `.nnue` file. Ravager-format and Leorik-format nets are auto-detected. Empty reloads the embedded net (if built with `EVALFILE`). |
| `SyzygyPath` | string | \<empty\> | Directory/directories of Syzygy `.rtbw`/`.rtbz` files. Probing activates only when tables are found. |
| `SyzygyProbeLimit` | spin | 6 | Only probe WDL during search when pieces on board ≤ limit (1–7; also capped by available tables). |
| `Syzygy50MoveRule` | check | true | Treat cursed wins / blessed losses as draws (the standard 50-move-aware mode). |

Environment conveniences: `RAVAGER_EVALFILE=<net>` preloads a net at startup,
`RAVAGER_NNUE_VERIFY=1` turns on the accumulator self-check (slow!).

## STRENGTH

Measured with cutechess-cli on this very machine (i3, 2+0.02 blitz,
150 games each, popularpos_lichess_v3 openings, LOS 100%):

| Match | Score | Elo |
|---|---|---|
| Ravager 2, texel-tuned HCE — vs Ravager 1 | 118–20–12 (82.7%) | **+271 ± 70** |
| Ravager 2, NNUE (Leorik net) — vs Ravager 1 | 143–4–3 (96.3%) | **+568 ± 178** |

The tuned HCE parameters were produced by `./tuner` on the Zurichess set
(see above); holdout error tracked train error throughout.

Baseline Ravager 1 was ~2380 CCRL-blitz. On this i3 the engine searches
~550 knps with HCE and ~230 knps with the 640-hidden SCReLU net.

Ideas studied and synthesized from the classic HCE engines: CREDITS.md.
Third-party code/data: Pyrrhic (MIT, tb/LICENSE), incbin.h
(graphitemaster, public domain), Leorik net file (lithander, MIT),
Zurichess quiet-labeled dataset.
Source: https://github.com/erensh27/Ravager
