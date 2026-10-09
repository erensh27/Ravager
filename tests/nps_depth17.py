import subprocess,re,json,sys,statistics,time
fens=['rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1','r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1','8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1','r3k2r/pp1n1ppp/2p1pn2/2bp4/2BPP1b1/2N2N2/PPQ2PPP/R3K2R w KQkq - 0 1']
def run(exe):
 p=subprocess.Popen([exe],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1)
 def send(s):p.stdin.write(s+'\n');p.stdin.flush()
 def wait(prefix):
  while True:
   s=p.stdout.readline().strip()
   assert s or p.poll() is None,(exe,p.returncode)
   if s.startswith(prefix):return s
 send('uci');wait('uciok');send('setoption name Hash value 256');send('setoption name Threads value 1');send('isready');wait('readyok');samples=[]
 for fen in fens:
  send('ucinewgame');send('isready');wait('readyok');send('position fen '+fen);send('go depth 17 movetime 600000');last=None
  while True:
   s=p.stdout.readline().strip()
   if s.startswith('info depth '): last={k:int(re.search(r'\b'+k+r' (\d+)',s)[1]) for k in ['depth','nodes','time']}
   if s.startswith('bestmove'):break
  assert last['depth']==17,last;samples.append(last)
 send('quit');p.wait();assert p.returncode==0
 nodes=sum(s['nodes'] for s in samples);ms=sum(s['time'] for s in samples)
 assert nodes==3359287,(exe,nodes)
 return dict(exe=exe,nodes=nodes,time=ms,nps=nodes*1000/ms,samples=samples)
a,b=sys.argv[1:3];results=[]
for order in [(a,b),(b,a),(a,b)]:
 for exe in order:
  r=run(exe);results.append(r);print(json.dumps(r),flush=True)
summary={exe:statistics.median(r['nps'] for r in results if r['exe']==exe) for exe in [a,b]}
print('MEDIANS',json.dumps(summary),flush=True)
json.dump(dict(results=results,medians=summary),open(sys.argv[3],'w'),indent=2)

