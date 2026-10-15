# FRC and SMP verification

Kaggle CPU, October 9, 2026. Intel Xeon 2.20 GHz, 2 physical cores / 4 logical
CPUs, 4-CPU quota, AVX2 runtime inference. Based on commit
8fbd28b2994f321d74ebec9563fd45d255ccaa1b, embedded v109 network unchanged.

- Standard perft: 4,865,609 at startpos depth 5; 97,862 at Kiwipete depth 3;
  43,238 at rook/pawn ending depth 4. Unchanged.
- Single-thread built-in bench: exactly 43,427 nodes.
- All 960 numbered starts at depth 2, 12 starts at depth 3, 1,920 stripped
  castling arrangements at depth 2, random attacked-path cases and 150 FRC
  middlegames at depth 2: exact differential rules results.
- 20 FRC games, 4 workers, up to 24 plies per game: all moves legal.
- Regenerated 5,232-position fixture vs the unchanged pre-feature binary:
  scalar/AVX2/AVX512 all byte-identical eval dumps, zero mismatches. This is
  a regenerated regression fixture, not the original external-oracle suite.
  Fixture SHA256: c26cc413e434086ffc78f8b6f1aca6e238120b7a8f0a0bd330ef16547bbf60cd.
- Existing 28,802 incremental/full/scalar checks per tier: PASS. Existing
  special-move ASan/UBSan and failed-load atomicity: PASS.
- Crossing/overlapping/stationary FRC castles: ASan/UBSan, board/hash/PSQT
  make/unmake and incremental NNUE consistency: PASS.
- GCC TSan and ASan/UBSan: 12 cycles of 1-4 workers, Hash resize, newgame,
  isready during search, stop and clean quit: PASS. Non-PIE TSan used for
  final repeat because PIE TSan occasionally failed at startup with an
  unexpected-memory-mapping runtime error, before engine initialization.

## NPS scaling

Same isolated Kaggle session, Hash 256 MB, 2-second `go infinite` then `stop`.
Three positions, each repeated twice, six samples per count after one warmup.
Final aggregate worker counters divided by engine elapsed milliseconds.
No other tests or compilation ran during this final measurement.

| Threads | Median NPS | Relative to 1 |
| ------- | ---------- | ------------- |
| 1 | 150866 | 1.000x |
| 2 | 290512 | 1.926x |
| 3 | 330376.5 | 2.190x |
| 4 | 382739.5 | 2.537x |

Exact samples and timing are in `smp_nps_kaggle.json`. Fractional medians are
from averaging the middle two integer samples. This measures throughput, not
playing strength. The 4096 advertised worker limit is not tested by launching
4096 threads on this small machine. Worker creation failures are reported.
