#!/usr/bin/env python3
"""Small deterministic engine regression suite; no third-party packages needed."""
import re, subprocess, sys
engine = sys.argv[1] if len(sys.argv) > 1 else './ravager'
def run(text=None, args=()):
    p = subprocess.run([engine, *args], input=text, text=True, capture_output=True, timeout=120)
    assert p.returncode == 0, p.stderr
    return p.stdout
for fen, depth, expected in [
    ('startpos', 5, 4865609),
    ('fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1', 3, 97862),
    ('fen 8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1', 4, 43238),
]:
    out = run(f'position {fen}\nperft {depth}\nquit\n')
    assert f'Total: {expected}\n' in out, out
    print(f'PASS perft depth={depth} nodes={expected}')
out = run('uci\nisready\nposition startpos\ngo depth 4\nquit\n')
assert 'id name Ravager Fury\n' in out and 'uciok\n' in out and 'readyok\n' in out
assert re.search(r'bestmove [a-h][1-8][a-h][1-8]', out), out
print('PASS UCI identity, handshake and NNUE/default search')
out = run('setoption name Use NNUE value false\nposition startpos\ngo depth 4\nquit\n')
assert re.search(r'bestmove [a-h][1-8][a-h][1-8]', out), out
print('PASS HCE fallback search')
assert 'Bench total nodes:' in run(args=('bench',))
print('PASS built-in bench')
