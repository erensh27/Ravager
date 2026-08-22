Ravager 1

Pure handcrafted-evaluation (HCE) chess engine.
No NNUE. No opening book. No endgame tablebases.

STRENGTH
  Estimated ~2380 on the CCRL-blitz scale (posterior 95% CI 2320-2445)
  Measured with cutechess-cli 1.5.1 against CCRL-rated anchors:

    CT800 1.46      CCRL 2708    24 games   3W  - 0D  - 21L   (12.5%)
    Amundsen 0.80   CCRL 2325   124 games  67W  -10D  - 47L   (58.1%)

  Time control 4s+0.04s, random 6-10 ply openings, both colors.
  Bayesian Elo fit: Davidson draw model, logistic MLE, Gaussian priors
  on anchor ratings, 20k Monte Carlo samples over the priors.

BUILD
  make                     native build (x86-64 with BMI2/POPCNT)
  make CFLAGS="-O3 -std=c11 -mbmi2 -mpopcnt"   portable BMI2 build

RUN
  ./ravager                UCI engine, Hash 256 MB default

TOOLS
  make tuner                                  texel tuner (rewrites src/params.c)
  ./ravager datagen <games> <ms> <seed> <file> self-play training data
  ./ravager bench                             fixed-depth benchmark
  ./ravager perft <depth>                     movegen verification

Ideas studied and synthesized from the classic HCE engines: CREDITS.md
Source: https://github.com/erensh27/Ravager
