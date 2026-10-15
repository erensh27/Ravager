#include "ravager.h"
#include "nnue.h"
#include <assert.h>
int main(void) {
 board_init_all();assert(nnue_load_file("nets/Ravager_NET.nnue.zst"));
 const char *fens[]={"4k3/8/8/8/8/8/8/5KR1 w G - 0 1", "4k3/8/8/8/8/8/8/6KR w H - 0 1", "4k3/8/8/8/8/8/8/R1K5 w A - 0 1", "4k3/8/8/8/8/8/8/3RK3 w D - 0 1", "5kr1/8/8/8/8/8/8/4K3 b g - 0 1"};
 for(unsigned j=0;j<sizeof(fens)/sizeof(*fens);j++) {
  Board b;parse_fen(&b,fens[j]);nnue_prepare_search(&b);
  MoveList ml;generate_moves(&b,&ml);int count=0;
  for(int i=0;i<ml.count;i++) if(move_is_castle(ml.moves[i])) {
   Move m=ml.moves[i];Board before=b;int eval=nnue_raw_output(&b,false);
   nnue_push_move(&b,m);make_move(&b,m);assert(nnue_selfcheck(&b));
   unmake_move(&b,m);assert(nnue_selfcheck(&b));assert(nnue_raw_output(&b,false)==eval);
   assert(!memcmp(b.pieces,before.pieces,sizeof b.pieces));assert(b.hash==before.hash);
   assert(!memcmp(b.psqt,before.psqt,sizeof b.psqt));
   assert(!memcmp(b.piece_on,before.piece_on,sizeof b.piece_on));count++;
  }
  assert(count==1);
 }
 puts("PASS overlapping/stationary FRC make/unmake and NNUE incremental");
}
