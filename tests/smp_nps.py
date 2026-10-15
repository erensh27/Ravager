import subprocess,time,queue,threading,re,json,statistics,os,sys
ENGINE=sys.argv[1] if len(sys.argv)>1 else './ravager'
fens=['rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1','r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1','r1bq1rk1/pp2bppp/2n1pn2/2pp4/3P4/2PBPN2/PP1N1PPP/R1BQ1RK1 w - - 0 8']
results={n:[] for n in [1,2,3,4]}
for trial in range(7):
 for n in [1,2,3,4]:
  p=subprocess.Popen([ENGINE],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
  q=queue.Queue()
  def reader(proc=p,que=q):
   for line in proc.stdout:que.put(line)
  threading.Thread(target=reader,daemon=True).start()
  p.stdin.write(f'uci\nsetoption name Hash value 256\nsetoption name Threads value {n}\nisready\n');p.stdin.flush()
  while not q.get(timeout=10).startswith('readyok'):pass
  p.stdin.write('position fen '+fens[trial%3]+'\ngo infinite\n');p.stdin.flush();start=time.monotonic()
  time.sleep(2);p.stdin.write('stop\n');p.stdin.flush()
  lines=[]
  while True:
   line=q.get(timeout=15);lines.append(line)
   if line.startswith('bestmove'):break
  elapsed=time.monotonic()-start
  final=[s for s in lines if 'info string smp' in s]
  assert final,lines
  m=re.search(r'nodes (\d+) time (\d+) nps (\d+)',final[-1]);nodes,ms,nps=map(int,m.groups())
  if trial>0:results[n].append({'nodes':nodes,'ms':ms,'nps':nps,'wall_ms':round(elapsed*1000)})
  p.stdin.write('quit\n');p.stdin.flush();p.wait(timeout=5);assert p.returncode==0,p.stderr.read()
  print('probe',trial,n,nodes,ms,nps,flush=True)
report={str(n):{'median_nps':statistics.median(s['nps'] for s in samples),'samples':samples} for n,samples in results.items()}
json.dump(report,open('/tmp/nps-results.json','w'),indent=2);print(json.dumps(report,indent=2),flush=True)
