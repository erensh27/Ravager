# Tests and benchmarks

- `make test`: perft fixtures (startpos, Kiwipete and rook/pawn endgame), UCI handshake/default search, HCE fallback and built-in bench.
- `tests/diagnostics.c`: perft/divide and built-in four-position bench implementation.
- `make orthodox-bench`: 86-position aggregate benchmark.
- `python3 tests/compare_bench.py --before before-0.txt before-1.txt --after after-0.txt after-1.txt`: exact per-position node/bestmove comparison plus median aggregate NPS.
- `tests/tuner.c`: Texel tuner, built by `make tuner`.
- `sh tests/run_nnue.sh`: NNUE checks with the bundled net.
- `tests/fixtures/syzygy/`: bundled small tablebase fixtures.

# NNUE verification

Run `sh tests/run_nnue.sh [net]` from the source tree. Requires GCC and libzstd. `RAVAGER_NNUE_TIER=scalar|avx2|avx512` forces an inference tier; unsupported tiers fall back to an available implementation.

- nnue_incremental.c: randomized legal moves and unmake, promotion/en-passant fixtures, lazy/fresh/scalar accumulator checks.
- special.c: forced castling both sides/colors, en passant, all promotions, promotion captures and king mirror changes, built with ASan/UBSan.
- nnue_loader.c: failed-load atomicity and reload.
- nnue_eval.c: evaluates FENs from stdin; used with `tests/oracle_compare.py` and `tests/fixtures/nnue5232.json`.

## Honest aggregate benchmark

Compile orthodox_bench.c with all engine sources except uci.c, plus nnue/nnue.c and nnue/inference.c,
including src/tb/tbprobe.c, using `-O3 -std=c11 -march=native -pthread -Isrc -Innue -lm -lzstd`.
Run it with the local net path and tests/orthodox86.txt.
Set `RAVAGER_NNUE_TIER=avx2` to compare AVX2 consistently.

The FENs are the orthodox-compatible subset of Pawnocchio's 100-position suite:
https://github.com/JonathanHallstrom/pawnocchio . Positions whose castling
rights require Chess960 were removed. No Chess960 implementation is added.

Settings: depth 11, single-threaded search, 256MB TT, cold TT and history for
EVERY position, no Syzygy. Timing is monotonic wall time inside search, excludes
load and reset time. Report aggregate nodes / aggregate search seconds, not
last-position info-line NPS. Check per-position nodes and bestmove identity.

## Chess960 and SMP regression checks

Install `python-chess` for differential rules checks.

- `python3 tests/chess960.py ./ravager`: all 960 numbered starts, exact perft
  against an independent rules library, plus stationary/overlapping castling,
  attacked paths and random FRC middlegames.
- `python3 tests/baseline_parity.py <old-engine> ./ravager`: regenerated 5,232
  positions compared to the unchanged pre-feature build, all inference tiers.
  This is a regression fixture, not the original external-oracle fixture.
- `python3 tests/smp_lifecycle.py <engine> [more-engines]`: repeated 1-4-thread
  search/stop/isready/newgame/option cycles. Use sanitizer builds as arguments.
- `python3 tests/smp_nps.py ./ravager`: same-session scaling, 2-second searches,
  one warmup and six samples per count, Hash 256 MB. Run without other CPU work.
- `tests/frc_special.c`: sanitizer/incremental NNUE checks for overlapping,
  crossing and stationary-king/rook castles.

The option's 4096-thread limit is tested by protocol bounds, not by launching
4096 threads on a four-logical-CPU machine.
