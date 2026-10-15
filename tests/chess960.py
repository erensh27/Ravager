import chess,subprocess,random,re,time,json,sys
ENGINE=sys.argv[1] if len(sys.argv)>1 else "./ravager"
random.seed(960)
def perft(b,d):
 if d==0:return 1
 if d==1:return b.legal_moves.count()
 n=0
 for m in list(b.legal_moves):
  b.push(m);n+=perft(b,d-1);b.pop()
 return n
p=subprocess.Popen(['stdbuf','-oL',ENGINE],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1)
p.stdin.write('setoption name UCI_Chess960 value true\n');p.stdin.flush()
def probe(fen,d):
 p.stdin.write('position fen '+fen+'\nperft '+str(d)+'\n');p.stdin.flush();moves={}
 while True:
  s=p.stdout.readline().strip()
  if s.startswith('Total:'):return int(s.split()[1]),moves
  m=re.fullmatch(r'([a-h][1-8][a-h][1-8][nbrq]?): (\d+)',s)
  if m:moves[m[1]]=int(m[2])
def check(b,d,label):
 fen=b.shredder_fen();actual,_=probe(fen,d);want=perft(b,d)
 assert actual==want,(label,fen,d,actual,want)
# Numbered setup must reproduce the independent start FEN's node counts.
for i in range(960):
 p.stdin.write('position startpos960 '+str(i)+'\nperft 2\n');p.stdin.flush()
 while True:
  line=p.stdout.readline().strip()
  if line.startswith('Total:'):break
 assert int(line.split()[1])==perft(chess.Board.from_chess960_pos(i),2),i
for i in range(960):
 b=chess.Board.from_chess960_pos(i);check(b,2,i)
 if i%80==0:check(b,3,i)
 # All arrangements of castling pieces with other men removed.
 for c in [chess.WHITE,chess.BLACK]:
  b=chess.Board.from_chess960_pos(i)
  for sq in list(b.piece_map()):
   pc=b.piece_at(sq)
   if pc.piece_type not in (chess.KING,chess.ROOK) or pc.color!=c and pc.piece_type!=chess.KING:b.remove_piece_at(sq)
  b.turn=c;check(b,2,('castle',i,c))
# Check stationary, crossing and rook-unmasking attacks with extra enemy men.
for i in range(200):
 b=chess.Board.from_chess960_pos(random.randrange(960))
 for sq,pc in list(b.piece_map().items()):
  if pc.piece_type not in (chess.KING,chess.ROOK):b.remove_piece_at(sq)
 for j in range(3):
  sq=random.randrange(8,56)
  if not b.piece_at(sq):b.set_piece_at(sq,chess.Piece(random.choice([chess.ROOK,chess.BISHOP,chess.KNIGHT]),chess.BLACK))
 if b.is_valid():check(b,2,('attacked castle',i))
for i in range(150):
 b=chess.Board.from_chess960_pos(random.randrange(960))
 for ply in range(random.randrange(5,60)):
  if b.is_game_over():break
  b.push(random.choice(list(b.legal_moves)))
 check(b,2,('random',i))
p.stdin.write('quit\n');p.stdin.flush();p.wait()
print('PASS all 960 starts depth2, 12 depth3, 1920 stripped castling positions depth2, 150 random middlegames depth2')
