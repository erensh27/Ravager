#!/usr/bin/env python3
"""Compare a Ravager build to a separately built reference oracle using FENs.
The oracle must embed the EXACT same official net. In-check positions are
excluded: The oracle's raweval command deliberately returns zero in check.
"""
import argparse,json,re,subprocess,statistics,os
p=argparse.ArgumentParser()
p.add_argument('--oracle',required=True);p.add_argument('--engine',required=True)
p.add_argument('--fens',required=True);p.add_argument('--net',required=True)
a=p.parse_args();fens=json.load(open(a.fens))
proc=subprocess.Popen([a.oracle],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1)
proc.stdin.write('uci\n');proc.stdin.flush()
while proc.stdout.readline().strip()!='uciok':pass
expected=[]
for fen in fens:
 proc.stdin.write('position fen '+fen+'\nraweval\n');proc.stdin.flush()
 while True:
  line=proc.stdout.readline().strip()
  if re.fullmatch(r'-?\d+',line):expected.append(int(line));break
proc.stdin.write('quit\n');proc.stdin.flush();proc.wait()
for tier in ['scalar','avx2','avx512']:
 cmd='uci\n'+''.join('position fen '+f+'\nd\n' for f in fens)+'quit\n'
 result=subprocess.run([a.engine],input=cmd,text=True,capture_output=True,timeout=180,env=dict(os.environ,RAVAGER_NNUE_TIER=tier,RAVAGER_EVALFILE=a.net))
 assert result.returncode==0,result.stderr
 actual=[int(x) for x in re.findall(r'Eval: (-?\d+)',result.stdout)]
 assert len(actual)==len(expected),(len(actual),len(expected))
 diff=[abs(x-y) for x,y in zip(expected,actual)]
 print(tier,'positions',len(actual),'max',max(diff),'mean',statistics.mean(diff),'mismatches',sum(bool(x) for x in diff))
 assert not any(diff)
