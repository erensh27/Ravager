# NNUE verification

Kaggle CPU, portable x86-64-v2 build, libzstd statically linked.

- 5,232 oracle positions x scalar/AVX2/AVX512: max difference 0, mean 0, mismatches 0.
- 28,802 incremental/full/scalar checks per tier; ASan/UBSan special moves PASS.
- Failed-load atomicity PASS.
- Regression: perft/UCI/NNUE/HCE/bench PASS.
