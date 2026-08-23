# CREDITS — Ravager 1

Ravager 1 is a pure handcrafted-evaluation (HCE) chess engine: no NNUE,
no opening book, no endgame tablebases. It was written from scratch; no code
was copied from any engine. What was borrowed is knowledge — the publicly
documented ideas, heuristics and tuning philosophy of the great handcrafted
engines below, re-derived and hand-integrated for Ravager's own architecture.

## Thank you

- **Tord Romstad, Marco Costalba, Joona Kiiski, and GM Larry Kaufman**, authors of
  **Stockfish 11** — the last handcrafted-eval Stockfish. The backbone of Ravager's
  king-safety attack counting, mobility areas excluding enemy pawn control, pawn
  hashtable, aspiration windows, LMR log-log tables, singular extensions,
  continuation history, PEXT slider attacks, and the `improving` heuristic all
  trace to the Stockfish tradition.
- **Don Dailey, GM Larry Kaufman and Mark Lefler**, authors of **Komodo 13** — for
  the king-safety and outpost philosophy, aggressive pruning, and above all the
  predictive time management Ravager uses (branching-factor iteration prediction,
  bestmove instability, easy-move detection).
- **Robert Houdart**, author of **Houdini 6** — for adaptive null-move pruning with
  verification, ProbCut-style pre-pruning, and pioneering use of singular
  extensions.
- **Andrew Grant**, author of **Ethereal 11.00** — the cleanest modern HCE reference:
  tapered PST design, pawn-structure hashing, threat evaluation, and the history
  gravity formula. Ethereal's architecture was the closest structural inspiration.
- **Stefan Meyer-Kahlen**, author of **Deep Shredder 13** — for passed-pawn
  evaluation with king-distance races, pawn-storm logic, and endgame drawishness
  scaling.
- **Vasik Rajlich**, author of **Rybka 4.1** — for the tapered-evaluation formulation
  and king-attack counting tradition.
- **Vadim Demichev**, author of **Gull 3.0** — for tapered evaluation and space
  terms, and the incremental-evaluation accumulator approach.
- **Peter Österlund**, author of **Texel 1.07** — for the texel tuning method
  (logistic-loss optimisation of evaluation parameters over game data) that tuned
  Ravager's evaluation; also SEE usage in quiescence.
- **Youri Matiounine**, author of **Fizbo 2** — for the razoring/futility pruning
  family and evaluation-weight tuning style.
- **Daniel José Queraltó**, author of **Andscacs 0.94** — for the fully-legal
  move-generation style (checkmask/pins) and quiet-move pruning ideas.
- **K dayBird Young**, author of **Chiron 4** — for king-safety scaling and space
  evaluation ideas.
- **Alex Morozov**, author of **Booot 6.4** — for pawn-structure detail and rook
  evaluation.
- **Morgan Houppin**, author of **Stash v30** — for the compact modern HCE design
  and razoring/PST style.
- **Jeffrey An**, author of **Laser 1.7** — for futility/LMP calibration and a
  clean search structure to measure against.
- The **Fire** authors — for late-move-pruning and move-ordering aggression.

## Technique map

| In Ravager 1 | Primary idea source | Also used by |
|---|---|---|
| Tapered evaluation (MG/EG + phase) | Rybka 4.1, Gull 3.0 | Stockfish 11, Ethereal 11, Laser 1.7, Texel 1.07 |
| Tapered PSTs + material in params.c | Ethereal 11.00, Gull 3.0 | Stockfish 11, Laser 1.7, Stash v30 |
| Incremental PSQT/material/phase accumulators | Gull 3.0, Stockfish 11 | Ethereal 11.00, Laser 1.7 |
| Pawn-structure hash cache | Stockfish 11, Ethereal 11.00 | Laser 1.7, Andscacs 0.94 |
| Mobility excluding enemy-pawn-controlled squares | Stockfish 11 | Komodo 13, Fire, Ethereal 11.00 |
| Non-linear king-safety table | Stockfish 11 | Rybka 4.1, Houdini 6, Chiron 4 |
| Pawn shield / pawn storm | Deep Shredder 13, Komodo 13 | Booot 6.4, Chiron 4 |
| Passed pawns with king-distance races | Deep Shredder 13, Komodo 13 | Texel 1.07, Gull 3.0 |
| Outposts (knight/bishop, pawn-safe) | Komodo 13, Ethereal 11.00 | Houdini 6, Laser 1.7 |
| Threat evaluation | Ethereal 11.00 | Fizbo 2, Andscacs 0.94 |
| Space evaluation | Komodo 13, Gull 3.0 | Chiron 4, Stash v30 |
| Material drawishness scaling (OCB, rook endings, wrong-color bishop) | Deep Shredder 13, Komodo 13 | Stockfish 11, Gull 3.0 |
| Tempo term | classical HCE | all |
| PVS + aspiration windows | Stockfish 11 | Komodo 13, Houdini 6, Ethereal 11.00 |
| TT: depth-preferred/always-replace buckets, ageing | Stockfish 11 | all |
| Killers, history, countermove, continuation history (gravity) | Stockfish 11, Ethereal 11.00 | Laser 1.7, Stash v30 |
| LMR log-log table + corrections | Stockfish 11, Houdini 6 | Komodo 13, Fire, Ethereal 11.00 |
| Reverse futility / futility / razoring / LMP | Stockfish 11, Fizbo 2, Fire | Ethereal 11.00, Laser 1.7, Stash v30 |
| Adaptive NMP with verification | Houdini 6, Stockfish 11 | Komodo 13, Fire |
| ProbCut-style pre-pruning | Houdini 6 | Komodo 13 |
| Singular + double-check extensions | Stockfish 11 | Houdini 6, Komodo 13 |
| `improving`-scaled margins | Stockfish 11 | Fire, Laser 1.7 |
| IIR, mate-distance pruning | Stockfish 11 | Ethereal 11.00, Laser 1.7, Chiron 4 |
| SEE swap-list, SEE pruning in qsearch, SEE capture ordering | Stockfish 11, Texel 1.07 | all |
| Pseudo-legal movegen + verify-after-make (legal evasion generator for qsearch) | Andscacs 0.94, Ethereal 11.00 | Laser 1.7, Stash v30 |
| BMI2/PEXT slider attacks | Stockfish 11 | Fire, Fizbo 2 |
| Predictive time management | Komodo 13 | Stockfish 11, Ethereal 11.00 |
| Texel tuning pipeline (datagen + tuner) | Texel 1.07 | Ethereal 11.00, Laser 1.7, Stash v30 |

## Measurement

The strength estimate in README.md was measured with cutechess-cli against
CT800 1.46 and Amundsen 0.80 (CCRL 40/4 blitz anchors) and fitted with a
Bayesian Elo model (Davidson draws, anchor priors). Many thanks to the CCRL
for maintaining the public rating lists that make such calibration possible,
and to **Rasmus Althoff** (CT800) and **John Bergbom** (Amundsen) for their engines.

If any attribution above is inaccurate, please open an issue on the repository
and it will be corrected.

---

# Ravager 2 additions

Third-party code and data used by Ravager 2 (all permissively licensed):

- **Pyrrhic** by basil, Jon Dart and Andrew Grant — Syzygy tablebase probing
  (WDL during search, DTZ at the root). Vendored under `src/tb/`, MIT licence
  in `tb/LICENSE`. https://github.com/AndyGrant/Pyrrhic
- **incbin.h** by Dale Weister (graphitemaster) — compile-time embedding of
  NNUE nets. Public domain / unlicense-style header.
  https://github.com/graphitemaster/incbin
- **Leorik** by Thomas Jahn (lithander) — the bundled `nets/*.nnue` files come
  from Leorik's public releases; Ravager 2 implements Leorik's bucketed
  SCReLU net format natively (verified score-for-score against an independent
  implementation). MIT. https://github.com/lithander/Leorik
- **Zurichess quiet-labeled dataset** by Alexandru Moșoi — 725k quiet,
  resolved positions (`data_quiet.epd`) used for texel tuning; the same set
  behind many classic HCE tunings (Ethereal, Rofchade, PeSTO ...).
  https://bitbucket.org/zurichess/tuner (mirrored on GitHub)
- **Ronald de Man** — creator of the Syzygy tablebase format itself.

The NNUE integration follows the standard accumulator design popularised by
Stockfish NNUE (Yu Nasu, Nasu; and the Stockfish team) and documented for
small C engines in Berserk (Jay Honnold), Leorik, and Marvin.
