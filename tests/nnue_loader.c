#include "nnue.h"
#include "bitboard.h"
#include <assert.h>
int main(int argc,char **argv){assert(argc==2);board_init_all();Board b;parse_fen(&b,"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");assert(nnue_load_file(argv[1]));nnue_prepare_search(&b);int v=nnue_raw_output(&b,false);assert(!nnue_load_file("/does-not-exist"));assert(nnue_raw_output(&b,false)==v);assert(nnue_selfcheck(&b));assert(nnue_load_file(argv[1]));nnue_prepare_search(&b);assert(nnue_raw_output(&b,false)==v);puts("failed-load atomicity PASS");return 0;}
