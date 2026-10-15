#include "nnue.h"
#include "bitboard.h"
#include <assert.h>
static void run(const char *fen,const char *text){
 Board b;parse_fen(&b,fen);nnue_prepare_search(&b);assert(nnue_selfcheck(&b));
 MoveList ml;generate_moves(&b,&ml);Move selected=NO_MOVE;
 for(int i=0;i<ml.count;i++)if(!strcmp(move_to_str(ml.moves[i]),text))selected=ml.moves[i];
 assert(selected!=NO_MOVE);int side=b.side;int32_t before=nnue_raw_output(&b,false);
 nnue_push_move(&b,selected);make_move(&b,selected);assert(!is_in_check(&b,side));assert(nnue_selfcheck(&b));
 unmake_move(&b,selected);assert(nnue_selfcheck(&b));assert(nnue_raw_output(&b,false)==before);
 /* Skipped evaluation, then a null frame. */
 nnue_push_move(&b,selected);make_move(&b,selected);
 if(!is_in_check(&b,b.side)){nnue_push_null(&b);make_null_move(&b);assert(nnue_selfcheck(&b));unmake_null_move(&b);assert(nnue_selfcheck(&b));}
 unmake_move(&b,selected);assert(nnue_selfcheck(&b));
 printf("PASS %s\n",text);
}
int main(int argc,char **argv){if(argc!=2)return 2;board_init_all();if(!nnue_load_file(argv[1]))return 3;
 run("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1","e1g1");
 run("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1","e1c1");
 run("r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1","e8g8");
 run("r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1","e8c8");
 run("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1","e5d6");
 run("4k3/8/8/8/3Pp3/8/8/4K3 b - d3 0 1","e4d3");
 const char *white="4k3/P7/8/8/8/8/8/4K3 w - - 0 1";
 run(white,"a7a8q");run(white,"a7a8r");run(white,"a7a8b");run(white,"a7a8n");
 const char *black="4k3/8/8/8/8/8/p7/4K3 b - - 0 1";
 run(black,"a2a1q");run(black,"a2a1r");run(black,"a2a1b");run(black,"a2a1n");
 run("1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1","a7b8q");
 run("4k3/8/8/8/8/8/p7/1R2K3 b - - 0 1","a2b1n");
 run("7k/8/8/8/8/8/8/3K4 w - - 0 1","d1e1");
 run("3k4/8/8/8/8/8/8/7K b - - 0 1","d8e8");
 return 0;}
