#include "bitboard.h"
#include "search.h"
#include "tt.h"
#include "nnue.h"
#include <time.h>
int move_overhead_ms=20;
static double stamp(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char **argv){
 if(argc!=3)return 2;
 board_init_all();init_lmr_table();tt_alloc(256);search_verbose=0;
 if(!nnue_load_file(argv[1]))return 3;
 FILE *f=fopen(argv[2],"r");if(!f)return 4;
 char fen[512];uint64_t total=0;double secs=0;int n=0;Board b;
 while(fgets(fen,sizeof fen,f)){
  parse_fen(&b,fen);tt_clear();search_reset_tables();search_set_game_history(NULL,0);
  search_max_depth=11;search_soft_ms=search_hard_ms=0x3fffffff;
  double start=stamp();search_new_job(); search_iterative_deepening(&b);double elapsed=stamp()-start;
  printf("%d %llu %.6f %u\n",++n,(unsigned long long)search_nodes,elapsed,search_root_best);fflush(stdout);
  total+=search_nodes;secs+=elapsed;
 }
 fprintf(stderr,"positions=%d nodes=%llu seconds=%.6f aggregate_nps=%.0f tier=%s hash=256 depth=11 cold_per_position=yes\n",n,(unsigned long long)total,secs,total/secs,nnue_tier);
 fclose(f);return 0;
}
