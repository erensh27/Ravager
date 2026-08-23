#!/usr/bin/env python3
"""verify_leorik.py — independent reference implementation of Leorik's NNUE
forward pass (from Leorik.Core sources), used to validate Ravager 2's port.

Usage: python3 verify_leorik.py <net.nnue> [FEN ...]
Prints the reference centipawn eval (side-to-move POV) per position.
"""

import struct
import sys

QA, QB, SCALE = 255, 64, 400
INPUT_SIZE = 768

BUCKET_MAP = [
    0,0,1,1,1,1,0,0,
    2,2,3,3,3,3,2,2,
    2,2,3,3,3,3,2,2,
] + [4] * 40

PIECE_CHARS = {'P':(0,True),'N':(1,True),'B':(2,True),'R':(3,True),
               'Q':(4,True),'K':(5,True),
               'p':(0,False),'n':(1,False),'b':(2,False),'r':(3,False),
               'q':(4,False),'k':(5,False)}

def parse_fen(fen):
    board = {}   # sq -> (type, is_white)
    parts = fen.split()
    sq = 56
    for ch in parts[0]:
        if ch == '/': sq -= 16
        elif ch.isdigit(): sq += int(ch)
        else:
            t, w = PIECE_CHARS[ch]
            board[sq] = (t, w)
            sq += 1
    stm = 0 if parts[1] == 'w' else 1   # 0 white, 1 black
    return board, stm

def load_net(path):
    data = open(path, 'rb').read()
    off = 0
    def read_shorts(n):
        nonlocal off
        vals = struct.unpack_from(f'<{n}h', data, off)
        off += n * 2
        return vals
    # dims from the standard 640HL-S-5io8 layout; adjust here if needed
    H, K, O = 640, 5, 8
    ft_w = [read_shorts(H) for _ in range(K * INPUT_SIZE)]
    ft_b = list(read_shorts(H))
    out_w = []
    for _ in range(O * 2):
        out_w.append(list(read_shorts(H)))
    out_b = list(read_shorts(O))
    assert off <= len(data), "net file shorter than expected"
    return ft_w, ft_b, out_w, out_b

def feature_index(acc_bucket, acc_mirror, persp, pt, color, sq):
    # color: True=white ; persp: 0=white-view, 1=black-view
    if persp == 0:
        sub = 0 if color else 384
        fsq = sq ^ (7 if acc_mirror[0] else 0)
    else:
        sub = 384 if color else 0
        fsq = sq ^ (63 if acc_mirror[1] else 56)
    return acc_bucket[persp] * INPUT_SIZE + sub + pt * 64 + fsq

def evaluate(net, fen):
    ft_w, ft_b, out_w, out_b = net
    board, stm = parse_fen(fen)

    wk = next(s for s,(t,w) in board.items() if t==5 and w)
    bk = next(s for s,(t,w) in board.items() if t==5 and not w)

    bucket = [BUCKET_MAP[wk], BUCKET_MAP[bk ^ 56]]
    mirror = [(wk & 7) >= 4, ((bk ^ 56) & 7) >= 4]

    acc = [[ft_b[i]]*2 for i in range(640)]
    acc = [list(ft_b), list(ft_b)]
    for s,(t,w) in board.items():
        for p in (0,1):
            idx = feature_index(bucket, mirror, p, t, w, s)
            row = ft_w[idx]
            ap = acc[p]
            for i in range(len(row)):
                ap[i] += row[i]

    pc = len(board)
    div = -(-32 // 8)          # ceil
    ob = min(max((pc - 2) // div, 0), 7)

    us, them = acc[stm], acc[stm ^ 1]
    total = 0
    for i in range(640):
        c = us[i]
        c = 0 if c < 0 else (QA if c > QA else c)
        total += c * c * out_w[ob * 2][i]
    for i in range(640):
        c = them[i]
        c = 0 if c < 0 else (QA if c > QA else c)
        total += c * c * out_w[ob * 2 + 1][i]

    # C-style truncation toward zero everywhere
    q1 = abs(total) // QA
    if total < 0: q1 = -q1
    out = q1 + out_b[ob]
    q2 = abs(out) * SCALE // (QA * QB)
    if out < 0: q2 = -q2
    return q2

if __name__ == '__main__':
    net = load_net(sys.argv[1])
    for fen in sys.argv[2:]:
        print(f"{evaluate(net, fen):6d}  {fen}")
