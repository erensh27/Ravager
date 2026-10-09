#!/usr/bin/env python3
"""Compare orthodox_bench output files without treating time noise as chess changes."""
import argparse, statistics
p=argparse.ArgumentParser()
p.add_argument('--before',nargs='+',required=True)
p.add_argument('--after',nargs='+',required=True)
a=p.parse_args()
def read(path):
    rows=[line.split() for line in open(path) if line.strip()]
    assert len(rows)==86, (path,len(rows))
    sig=[(r[0],r[1],r[3]) for r in rows]
    nodes=sum(int(r[1]) for r in rows)
    seconds=sum(float(r[2]) for r in rows)
    return sig,nodes/seconds
reference=read(a.before[0])[0]
before=[];after=[]
for group,files in [(before,a.before),(after,a.after)]:
    for f in files:
        sig,nps=read(f)
        assert sig==reference, f'Node/bestmove mismatch in {f}'
        group.append(nps)
b=statistics.median(before);c=statistics.median(after)
print('All 86 per-position nodes and bestmoves match.')
print('before_nps', [round(x) for x in before])
print('after_nps', [round(x) for x in after])
print(f'median_before={b:.0f} median_after={c:.0f} change={(c/b-1)*100:.3f}%')
