# Single-thread TT fast path

October 9, 2026, separate sprynn Kaggle CPU development session.
Base: 10356ac905bc7e56fe19a6f653d4e4434b1f7dfe. Main comparison:
8fbd28b2994f321d74ebec9563fd45d255ccaa1b. Embedded network unchanged.

Only the TT fast path was retained. The concurrency mode is set by the search
coordinator before helper creation, held constant until every helper joins,
and reset afterwards. At one worker, no other thread accesses the TT. At more
than one worker, probes use locked snapshots and stores/hashfull use the same
4096 striped mutexes as before. UCI option changes/reset still wait for search
completion. Atomic stop signaling and private worker histories are unchanged.

## Gates

- Bench: exactly 43,427 nodes. Standard perft: 4,865,609 / 97,862 / 43,238.
- UCI handshake, NNUE/default search, HCE fallback: PASS.
- Tracked 5,232-position fixture: pre-change vs final byte-identical evals in
  scalar, AVX2 and AVX512. The separately regenerated 5,232-position fixture
  also passes all tiers (same generator/hash as FRC_SMP_VERIFICATION.md).
- Incremental/full/scalar: 28,802 checks per SIMD tier. Failed-load atomicity.
- All 960 starts, 12 depth-3 starts, 1,920 stripped castling arrangements,
  attacked castling paths and 150 random FRC middlegames: differential PASS.
- ASan/UBSan special moves, overlapping/stationary FRC make/unmake and NNUE,
  and 12 multithread lifecycle cycles: PASS.
- Non-PIE TSan: 12 lifecycle cycles, threads 1-4, Hash resize, newgame,
  isready during search, stop, quit: PASS, zero race reports. First non-PIE
  launch still encountered an ASLR startup mapping collision; next launch
  passed. A startup mapping failure is not a successful sanitizer run.

## NPS and attribution

Every depth-17 pass: fresh process, Threads 1, Hash 256, four FENs,
ucinewgame each, go depth 17 movetime 600000. Serial alternating A/B, B/A,
A/B, three passes per binary. All passes exactly 3,359,287 nodes and depth 17.
Medians below are nodes * 1000 / summed engine milliseconds. Raw results and
FENs are committed with the harness. Shared-host variation is visible; these
small samples do not establish a precise universal recovered percentage.

- Isolated TT change: pre 206,382.44, TT 208,949.87 NPS (+1.24%).
- Final shaped build repeat: pre 196,840.91, final 203,309.75 (+3.29%).
- Main vs final: main 209,366.59, final 208,703.22 (-0.32%).
- Relaxed abort atomics: TT 208,975.86, relaxed 207,056.64 (-0.92%). Rejected.
- No-helper-allocation variant: TT 208,884.90, variant 206,560.11 (-1.11%).
  Rejected. No TLS or coordinator change was retained or attributed.
- Quick 2-second scaling check, three positions after warmup: 1T 130,828,
  2T 229,430 NPS (1.754x). Throughput, not playing strength.

Earlier 247k-session results are not directly comparable to this host session.
