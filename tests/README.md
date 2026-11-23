Run `make test` from the repository root. These deterministic checks do not play games or run strength tests.

- Search: evasion coverage, late captures, promotions, repetition boundaries, mate precedence, quiescence bounds, decisive-score normalization, pinned exchanges, thread voting, `searchmoves` root filtering and MultiPV analysis runs.
- Board: reference perft counts, evasion-generator agreement and make/unmake restoration, including all 960 starting arrangements.
- NNUE: incremental versus fresh accumulators, scalar versus available SIMD heads, every sparse-index mask, lazy/null paths, castling, en passant, promotions, repeated roots and cache invalidation after reload. Reload testing changes only a temporary in-memory network copy.
- Tablebase adapter: en-passant arguments for both colours and absent en passant, without tablebase files.

The tests include internal implementation files to exercise search and inference invariants without adding a test API to the engine. Assertions remain enabled in the test translation units.

For a Linux sanitizer build in a separate checkout, use:

```sh
make -B -j4 test CC=gcc ARCH_FLAGS=-march=x86-64-v3 \
  CFLAGS='-O1 -g -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer' \
  LDFLAGS=-fsanitize=address,undefined
```

Correctness checks and node-throughput measurements do not establish an Elo gain. Search changes still require playing-strength validation.
