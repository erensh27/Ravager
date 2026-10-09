#include "nnue.h"
#include "bitboard.h"
int main(int argc,char **argv){board_init_all();if(!nnue_load_file(argv[1]))return 2;Board b;parse_fen(&b,"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");unsigned state=17453,n=0;Move hist[130];int depth=0;
for(int game=0;game<100;game++){
 parse_fen(&b,game%3?"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1":"4k3/1P4p1/8/2pP4/8/8/6P1/4K3 w - c6 0 1");nnue_prepare_search(&b);depth=0;
 for(int step=0;step<200;step++){
  if(!nnue_selfcheck(&b)){fprintf(stderr,"fail %u game%d step%d\n",n,game,step);return 3;}n++;
  state=state*1664525+1013904223;
  if(depth && (state%7==0 || depth>100)){unmake_move(&b,hist[--depth]);continue;}
  MoveList list;generate_moves(&b,&list);Move legal[256];int count=0;
  for(int i=0;i<list.count;i++){int mover=b.side;nnue_push_move(&b,list.moves[i]);make_move(&b,list.moves[i]);bool ok=!is_in_check(&b,mover);unmake_move(&b,list.moves[i]);if(ok)legal[count++]=list.moves[i];}
  if(!count)break;
  Move m=legal[state%count];nnue_push_move(&b,m);make_move(&b,m);hist[depth++]=m;
 }
 while(depth){unmake_move(&b,hist[--depth]);if(!nnue_selfcheck(&b))return 4;n++;}
}
printf("incremental/fresh/scalar cases %u PASS\n",n);return 0;}
