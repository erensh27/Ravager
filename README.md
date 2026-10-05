# Ravager Chess Engine

<p align="center">
  <img src="assets/ravager.png" alt="Ravager Chess Engine" width="100%"/>
</p>

<p align="center">
  <a href="#strength"><img alt="CCRL 40/15 rating: 3324" src="https://img.shields.io/badge/CCRL%2040%2F15-3324-d4a548"></a>
  <a href="LICENSE"><img alt="License MIT" src="https://img.shields.io/badge/license-MIT-4c8f56"></a>
  <img alt="Language" src="https://img.shields.io/badge/language-C11-5a7ea6">
  <img alt="Protocol" src="https://img.shields.io/badge/protocol-UCI-8a6fb8">
</p>

Superhuman chess engine written in C11.

---

## Installation & Downloads

### Prebuilt Binaries
Download precompiled binaries from the [GitHub Releases](https://github.com/erensh27/Ravager/releases/tag/v2.1) page (featuring **Ravager 2.1**). Download the executable for your platform and load it into any UCI-compliant chess GUI (such as Cutechess, Banksia, Arena, or Fritz).

### Building from Source
The default neural network (`nets/640HL-S-5io8-6116M-FRCv1.nnue`) is already embedded directly into the binary via `incbin`, so **no extra network download is required**.

```bash
# Clone the repository
git clone https://github.com/erensh27/Ravager.git
cd Ravager

# Build the native, self-contained binary (~5 MB)
make -j4

# Verify the installation with a benchmark
./ravager bench
```

#### Build Options
```bash
make EVALFILE=                    # Build HCE-only binary (~170 KB, no NNUE net)
make EVALFILE=path/to/net.nnue    # Embed a custom NNUE net
make tuner                        # Build the Texel tuner binary
```

---

## Strength

Official ratings on the [CCRL 40/15](https://computerchess.org.uk/4040/) benchmark list:

| Version | CCRL 40/15 | Notes |
|:---|:---:|:---|
| **Ravager 1** | — | Untested |
| **Ravager 2** | **3324** | Official CCRL 40/15 rating (Oct 2026) |
| **Ravager 2.1** | — | Untested |

---

## Features & Architecture

### Search
* **PVS & Deepening:** Principal Variation Search (PVS) with iterative deepening and dynamic aspiration windows.
* **Transposition Table:** Two-tier bucketed cache with age-penalized quality replacement and cache-line prefetching.
* **Move Ordering:** Transposition table move, winning & neutral captures ordered by Static Exchange Evaluation (SEE) and MVV-LVA, killer moves, countermoves, and history heuristics.
* **History Heuristics:** Multi-tier history combining butterfly history, 1-ply countermove history (`CTX_COUNTER`), 2-ply follow-up history (`CTX_FOLLOW`), and capture history with bonus/malus gravity updates.
* **Selectivity & Extensions:** Singular extensions with double extension and multi-cut detection, check extensions, and double-check extensions.
* **Reductions:** Late Move Reductions (LMR) informed by combined continuation history, and Internal Iterative Reductions (IIR) for missing or stale TT entries.
* **Pruning:** Adaptive Null Move Pruning (NMP), Late Move Pruning (LMP), Reverse Futility Pruning (RFP), razoring, futility pruning, and losing-capture SEE pruning.
* **Quiescence Search:** Dedicated quiescence search with check evasion generator, TT probing & storing, and delta pruning.

### Evaluation
* **Embedded NNUE:** Dual-perspective `(768→H)x2` accumulator with lazy king-bucket reconstruction, evaluating natively with zero external dependencies.
* **Network Format Support:** Supports both Ravager-format nets and Leorik-format SCReLU nets (`<H>HL-S-<K>io<O>-*.nnue`).
* **Handcrafted Fallback (HCE):** Comprehensive Texel-tuned evaluation with tapered game phases covering material, PSQT, mobility, pawn structures, passed pawns, rook on 7th rank, king safety contact checks, and weak-square control.
* **Endgame Tablebases:** Vendored Syzygy probing via Pyrrhic with in-search WDL lookups and DTZ-optimal root play (honoring the 50-move rule).

### Board Representation & Move Generation
* **Bitboard Architecture:** 64-bit board model utilizing hardware BMI2/PEXT instructions for slider attacks, with magic bitboards fallback.
* **Pseudo-Legal Move Generation:** Rapid pseudo-legal generation where move legality checks are paid only for moves that survive pruning and move ordering.
* **Legal Check Evasions:** Dedicated evasion generator emitting only legal responses to checks in quiescence search.
* **State & Hashing:** Incremental piece-square table (PSQT) tracking, incremental game phase counting, and 64-bit Zobrist hashing for positions, castling rights, and conditional en passant.

---

## UCI Options

| Option | Type | Default | Description |
|---|---|---|---|
| `Hash` | spin | 256 | Transposition table size in MB (1–65536). |
| `MoveOverhead` | spin | 20 | Safety margin in ms subtracted from engine time budgets. |
| `Ponder` | check | false | Ponder move emission support. |
| `Use NNUE` | check | true | Enable NNUE evaluation; set `false` for tuned HCE fallback. |
| `EvalFile` | string | *(embedded)* | Path to an external `.nnue` net (auto-detects format). Empty reloads embedded net. |
| `SyzygyPath` | string | *(empty)* | Directory path containing Syzygy `.rtbw` and `.rtbz` tablebase files. |
| `SyzygyProbeLimit` | spin | 6 | Piece count threshold for probing WDL during search. |
| `Syzygy50MoveRule` | check | true | Honor the 50-move draw rule in tablebase probing. |

---

## Tools

| Command | Purpose |
|---|---|
| `./ravager bench` | Run fixed-depth multi-position search benchmark |
| `./ravager perft <depth>` | Move generator speed and correctness verification |
| `./ravager datagen <games> <ms> <seed> <out>` | Generate self-play training data for evaluation tuning |
| `make tuner && ./tuner data.epd out.c [rounds]` | Run Texel tuning on labeled positions |
| `python3 tools/verify_leorik.py <net> <fens...>` | Independent NNUE forward-pass verification |

---

## Acknowledgments & Credits

* **Special thanks to [Leorik](https://github.com/lithander/Leorik)** by Thomas Jahn for providing the bundled neural network (`640HL-S-5io8-6116M-FRCv1.nnue`) and inspiring the network loader architecture.
* Transposition table, search heuristics, and evaluation design draw on published ideas from classic and modern engines including Stockfish, Komodo, Houdini, Ethereal, Shredder, Rybka, Gull, and Texel.
* Third-party bundled components and libraries:
  * [Pyrrhic](https://github.com/AndyGrant/Pyrrhic) (Syzygy tablebase probing) by basil, Jon Dart, and Andrew Grant.
  * [incbin.h](https://github.com/graphitemaster/incbin) (binary asset embedding) by Dale Weister.
  * Syzygy tablebases by Ronald de Man.

For full attribution details, please see [CREDITS.md](CREDITS.md).

---

## License

[MIT](LICENSE) © erensh27
