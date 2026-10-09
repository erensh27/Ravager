#include "nnue.h"
#include "bitboard.h"
int main(int argc,char **argv){board_init_all();if(!nnue_load_file(argv[1]))return 2;char line[1024];Board b;while(fgets(line,sizeof line,stdin)){if(!parse_fen(&b,line))return 3;nnue_prepare_search(&b);printf("%d %d\n",nnue_evaluate_board(&b),nnue_selfcheck(&b));}return 0;}
