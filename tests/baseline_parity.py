import chess,random,json,subprocess,re,os,hashlib,sys
BASELINE=sys.argv[1]; ENGINE=sys.argv[2]
rng=random.Random(5232);fens=[]
for w in range(64):
 for b in range(64):
  if b==w or chess.square_distance(w,b)<=1:continue
  pos=chess.Board(None);pos.set_piece_at(w,chess.Piece(chess.KING,chess.WHITE));pos.set_piece_at(b,chess.Piece(chess.KING,chess.BLACK));fens.append(pos.fen())
while len(fens)<5232:
 b=chess.Board.from_chess960_pos(rng.randrange(960)) if len(fens)%2 else chess.Board()
 for j in range(rng.randrange(8,100)):
  if b.is_game_over():break
  b.push(rng.choice(list(b.legal_moves)))
 if not b.is_check():fens.append(b.fen())
json.dump(fens,open('/tmp/parity5232.json','w'))
text='uci\n'+''.join('position fen '+fen+'\nd\n' for fen in fens)+'quit\n'
def dump(exe,tier):
 r=subprocess.run([exe],input=text,text=True,capture_output=True,env=dict(os.environ,RAVAGER_NNUE_TIER=tier),timeout=180)
 assert r.returncode==0,r.stderr
 v=re.findall(r'Eval: (-?\d+)',r.stdout);assert len(v)==5232,len(v)
 return v
ref=dump(BASELINE,'scalar')
for tier in ['scalar','avx2','avx512']:
 v=dump(ENGINE,tier);assert v==ref,(tier,sum(x!=y for x,y in zip(v,ref)))
 print('PASS regenerated 5232 baseline parity',tier,'mismatches 0',flush=True)
print('fixture sha256',hashlib.sha256(open('/tmp/parity5232.json','rb').read()).hexdigest(),flush=True)
