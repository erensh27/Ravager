# ♟️ Ravager Fury

<div align="center">
  <pre>
  _____                                      
 |  __ \                                     
 | |__) |__ ___   ____ _  __ _  ___ _ __     
 |  _  // _` \ \ / / _` |/ _` |/ _ \ '__|    
 | | \ \ (_| |\ V / (_| | (_| |  __/ |       
 |_|  \_\__,_| \_/ \__,_|\__, |\___|_|       
                          __/ |              
                         |___/               
  ______                   
 |  ____|                  
 | |__ _   _ _ __ _   _    
 |  __| | | | '__| | | |   
 | |  | |_| | |  | |_| |   
 |_|   \__,_|_|   \__, |   
                   __/ |   
                  |___/    
  </pre>
  
  *A UCI chess engine written in C11, pairing Ravager's own search with an embedded NNUE and a tuned handcrafted evaluation fallback.*
</div>

---

## 🚀 Installation

Release binaries embed the neural network, so there is no separate network file to download. The `ravager-frc-smp-linux-x86_64` asset on v3.2.0 combines full Chess960/FRC support, LazySMP (1 thread by default, maximum 4096), a shared Hash setting of 8-16384 MB, and the embedded CC0 network. 

**Quick Start for Users:**
- Download the Linux executable matching your CPU from the [releases page](https://github.com/erensh27/Ravager3/releases).
- Add the executable as a UCI engine in your chess GUI.
- On Linux, give the binary execute permission with `chmod +x <binary>`.

**Building from Source (Developers):**
1. Clone the repository:
   ```bash
   git clone https://github.com/erensh27/Ravager3.git
   ```
2. Build the engine:
   ```bash
   make -j$(nproc)
   ```

*Build Notes:*
- Requirements: GCC or Clang, Make, libzstd development headers/library, and Python 3 for the regression suite.
- The default local build uses `-march=native` on Linux x86-64 and embeds `nets/Ravager_NET.nnue.zst`.
- Portable AVX2/BMI2/POPCNT build: `make clean && make ARCH_FLAGS="-mavx2 -mbmi2 -mpopcnt"`.
- Zstandard is linked statically into the release binary.

## ⚔️ Strength

| Version | CCRL 40/15 |
| ------- | ---------- |
| Ravager 2 | 3324 +/-21 |
| Ravager Fury | TBD |

*Ravager 2.0 64-bit on the [CCRL 40/15 complete list](https://computerchess.org.uk/4040/rating_list_all.html), published October 2, 2026. Ravager Fury is left blank until it has an official rating.*

## 🧠 Architecture Details

Ravager Fury is an alpha-beta engine with a powerful neural-network evaluation at its core.

### 🔍 Search
- **Framework:** Principal Variation Search (PVS) with iterative deepening & dynamic aspiration windows
- **Transposition Table:** Bucketed replacement strategy by depth/age, mate-score adjustment, and memory prefetching
- **Move Ordering:** SEE/MVV-LVA capture ordering, killer moves, countermoves; history for butterfly, continuation, and captures
- **Extensions:** Check/double-check, singular/double extensions
- **Pruning & Reductions:** 
  - Multi-cut pruning, internal iterative reduction (IIR)
  - History-aware late-move reductions (LMR)
  - Adaptive verified null-move pruning & ProbCut-lite
  - Reverse futility pruning, razoring, late-move pruning, and quiet/capture SEE pruning
- **Quiescence Search:** Includes stand pat, captures/promotions, delta pruning, and legal check evasions
- **Endgame Handling:** Syzygy WDL/DTZ tablebase probing, fifty-move rule, insufficient material, mate-distance
- **LazySMP:** Independent root workers with private search/NNUE state and a shared, synchronized transposition table

### ♟️ Move & Board Representation
- **Structure:** 64-bit bitboards with PEXT and magic slider attack generation
- **State Management:** Incremental board state and Zobrist hashing
- **Variants:** Orthodox chess and full Chess960/FRC (handles overlapping/stationary castling)
- **FEN:** X-FEN and Shredder-FEN support for castling rights

### 🤖 Evaluation (NNUE)
- **Network:** CC0 network natively embedded in Linux binaries
- **Layers:** 
  - 1,024 int16 accumulator lanes per perspective
  - 16 physical king buckets with horizontal mirroring
- **Inputs:** Piece-square, threat, and same/adjacent-file pawn-pair inputs
- **Operations:** CReLU paired multiplication; sparse `1024 -> 32`; Hard-Swish-6, gated residual `32 -> 64 -> 32`; linear `32 -> 1`
- **Output:** Eight material-selected output buckets, exact float32/FMA inference
- **Optimization:** Scalar/AVX2/AVX512 runtime kernels with lazy incremental accumulators
- **Fallback:** Tuned handcrafted evaluation fallback when NNUE is off

*Read more in [docs/NNUE.md](docs/NNUE.md), [docs/SEARCH.md](docs/SEARCH.md), and [docs/CREDITS.md](docs/CREDITS.md).*

## ⚙️ UCI Options

| Option | Default | Range / purpose |
| ------ | ------- | --------------- |
| Threads | 1 | 1-4096 search workers (LazySMP) |
| Hash | 256 | 8-16384 MB, shared across workers |
| UCI_Chess960 | false | Enable Chess960 king-to-rook castling notation |
| MoveOverhead | 20 | 0-5000 ms |
| Use NNUE | true | Use neural evaluation; false selects the handcrafted fallback |
| EvalFile | empty | External compatible network; empty restores the embedded net |
| SyzygyPath | empty | Tablebase directory |
| SyzygyProbeLimit | 6 | 1-7 pieces |
| Syzygy50MoveRule | true | Respect the fifty-move rule in tablebase probing |

*(To play Chess960/FRC, ensure `UCI_Chess960` is enabled in your GUI).*

