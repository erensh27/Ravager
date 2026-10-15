import subprocess,threading,queue,time,os,chess,random,json,sys
for engine in sys.argv[1:] or ['./ravager']:
 p=subprocess.Popen([engine],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=open('/tmp/'+os.path.basename(engine)+'-stress.err','w'),text=True,bufsize=1)
 q=queue.Queue()
 def reader():
  for s in p.stdout:q.put(s)
 threading.Thread(target=reader,daemon=True).start()
 def send(s):p.stdin.write(s+'\n');p.stdin.flush()
 def await_(prefix,timeout=15):
  end=time.monotonic()+timeout
  while time.monotonic()<end:
   s=q.get(timeout=max(.01,end-time.monotonic()))
   if s.startswith(prefix):return s
  raise RuntimeError(prefix)
 send('uci');await_('uciok')
 for i in range(12):
  send('setoption name Hash value 8');send('setoption name Threads value '+str(1+i%4));send('ucinewgame');send('position startpos');send('go infinite')
  time.sleep(.05);send('isready');await_('readyok');send('stop');best=await_('bestmove')
  assert chess.Move.from_uci(best.split()[1]) in chess.Board().legal_moves,best
 send('quit');p.wait(timeout=10);assert p.returncode==0,p.returncode
 err=open('/tmp/'+os.path.basename(engine)+'-stress.err').read();assert not err,err
 print('PASS stress',engine,flush=True)
