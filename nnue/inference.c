/* Ravager network inference, written in C11.
 * The on-disk QuantisedNetwork is UNPERMUTED, little endian, with no header.
 * We keep natural FT neuron order and transpose L1 into our sparse kernel's
 * group-of-four layout. This avoids architecture-dependent file permutations.
 */
#include "inference.h"
#include "bitboard.h"
#include <immintrin.h>
#include <assert.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#define ravager_aligned_alloc(align, size) _aligned_malloc((size), (align))
#define ravager_aligned_free(ptr) _aligned_free(ptr)
#else
#define ravager_aligned_alloc(align, size) aligned_alloc((align), (size))
#define ravager_aligned_free(ptr) free(ptr)
#endif
#define WIDTH 1024
#define AUX 64368
#define PSQT 11264
#define CAP 512
#define FRAMES (MAX_PLY+16)
#define AUX_BYTES ((size_t)AUX*WIDTH)
#define PSQT_BYTES ((size_t)PSQT*WIDTH*2)
#define BIAS_OFFSET (AUX_BYTES+PSQT_BYTES)
#define L1_OFFSET (BIAS_OFFSET+WIDTH*2)
#define L1B_OFFSET (L1_OFFSET+1024*8*32)
#define L2_OFFSET (L1B_OFFSET+8*32*4)
#define L2B_OFFSET (L2_OFFSET+32*8*64*4)
#define L3_OFFSET (L2B_OFFSET+8*64*4)
#define L3B_OFFSET (L3_OFFSET+32*8*4)
_Static_assert(L3B_OFFSET+8*4 == NET_BYTES,"v111 format size");
static unsigned char *data;
static const int8_t *aw;
static const int16_t *pw,*bias;
static _Alignas(64) int8_t w1[8][32768];
static _Alignas(64) float b1[8][32],w2[8][32][64],b2[8][64],w3[8][32],b3[8];
const char *net_tier="scalar";
typedef struct {
 _Alignas(64) int16_t a[2][WIDTH];
 Bitboard attf[64];      /* full attack set of the non-king piece on each square, 0 elsewhere */
 Bitboard men,pawns;     /* non-king occupancy and pawn occupancy of this position */
 uint16_t pp[2][128];    /* pawn-pair feature ids per perspective */
 uint16_t ps[2][32];
 uint8_t npp[2],np[2];
 uint8_t squares[64], view[2], bucket[2];
 bool ready;
} Frame;
static _Thread_local Frame frames[FRAMES];
static _Thread_local int base_ply;
static _Thread_local uint64_t seen_old[(AUX+63)/64],seen_new[(AUX+63)/64];
static uint16_t square_offset[12][64],piece_span[12];
static uint32_t piece_base[12];
static uint8_t square_rank[12][64][64];
static int32_t key_base[12][12];static uint8_t key_dup[12][12];
static int16_t wrap(int v){return (int16_t)(uint16_t)v;}
static int clip(int v){return v<0?0:v>255?255:v;}
static Bitboard attacks(int piece,int sq,Bitboard occ){
 switch(piece%6){
 case PAWN:return pawn_attack_table[piece/6][sq];
 case KNIGHT:return knight_attack_table[sq];
 case BISHOP:return bishop_attacks(sq,occ);
 case ROOK:return rook_attacks(sq,occ);
 case QUEEN:return queen_attacks(sq,occ);
 default:return king_attack_table[sq];
 }
}
static int targets(int attacker,int victim){
 /* Allowed victim type ranks are part of the network's feature schema. */
 if(attacker==PAWN)return victim==KNIGHT?0:victim==ROOK?1:-1;
 if(attacker==KING || victim==KING)return -1;
 if((attacker==BISHOP || attacker==ROOK) && victim==QUEEN)return -1;
 return victim;
}
static int target_count(int t){return t==PAWN?4:t==KING?0:(t==BISHOP||t==ROOK)?8:10;}
static void feature_tables(void){
 uint32_t global=0;
 for(int p=0;p<12;p++){
  int n=0;
  for(int s=0;s<64;s++){
   square_offset[p][s]=n;
   Bitboard bb=attacks(p,s,0);
   for(int to=0;to<64;to++)square_rank[p][s][to]=popcount(bb&((1ULL<<to)-1));
   if(p%6!=PAWN || (s>=8 && s<56))n+=popcount(bb);
  }
  piece_base[p]=global;piece_span[p]=n;global+=n*target_count(p%6);
 }
 /* key = key_base[a][v] + square_offset[a][from] + square_rank[a][from][to]; -1 marks an absent feature.
  * A duplicate pair (same-type attack counted once) is dropped when from<to in the perspective's frame. */
 for(int a=0;a<12;a++)for(int v=0;v<12;v++){
  int local=targets(a%6,v%6);
  key_dup[a][v]=a%6==v%6 && (a/6!=v/6 || a%6!=PAWN);
  key_base[a][v]=local<0?-1:(int32_t)(4560+piece_base[a]+(v/6*(target_count(a%6)/2)+local)*piece_span[a]);
 }

 assert(global==59808);
}
static const unsigned char geometry[32]={0,1,2,3,4,5,6,7,8,9,10,11,8,9,10,11,12,12,13,13,12,12,13,13,14,14,15,15,14,14,15,15};
static unsigned king_bucket(const Board *b,int p){
 int king=lsb(b->pieces[p][KING])^(p?56:0), mirror=(king&7)>=4?7:0;
 return geometry[(king>>3)*4+((king^mirror)&7)];
}
static unsigned psqt_list(const Board *b,int p,uint16_t *out){
 int king=lsb(b->pieces[p][KING])^(p?56:0), mirror=(king&7)>=4?7:0;
 unsigned bucket=geometry[(king>>3)*4+((king^mirror)&7)],n=0;
 /* Mirrored effective buckets select the same 16 physical weight blocks. */
 for(int sq=0;sq<64;sq++){
  int pc=b->piece_on[sq];if(pc==EMPTY_SQUARE)continue;
  int type=pc%6,rel=(pc/6)^p;
  if(type==KING)rel=0;
  out[n++]=bucket*704+rel*384+type*64+(sq^(p?56:0)^mirror);
 }
 return n;
}
/* Threat edges of the attackers in `attackers` (non-king pieces of frame fr), as feature ids for perspective p. */
static int edge_list(const Frame *fr,Bitboard attackers,int p,uint32_t *out,int n){
 int fl=fr->view[p];
 for(Bitboard left=attackers&fr->men;left;left&=left-1){
  int from=__builtin_ctzll(left),ap=fr->squares[from];
  int a=(ap/6^p)*6+ap%6,ff=from^fl;
  for(Bitboard t=fr->attf[from]&fr->men;t;t&=t-1){
   int to=__builtin_ctzll(t),vp=fr->squares[to];
   int v=(vp/6^p)*6+vp%6,tt=to^fl;int32_t kb=key_base[a][v];
   if(kb>=0 && !(key_dup[a][v] && ff<tt)){assert(n<CAP);out[n++]=kb+square_offset[a][ff]+square_rank[a][ff][tt];}
  }
 }
 return n;
}
static void pawn_pairs(const Board *b,Frame *f){
 int flip[2];for(int p=0;p<2;p++){f->npp[p]=0;flip[p]=f->view[p];}
 int sqs[16],colors[16],n=0;Bitboard pawns=b->pieces[0][PAWN]|b->pieces[1][PAWN];
 while(pawns){int sq=lsb_pop(&pawns);assert(n<16);sqs[n]=sq;colors[n++]=b->piece_on[sq]/6;}
 for(int i=0;i<n;i++)for(int j=i+1;j<n;j++)if(abs((sqs[i]&7)-(sqs[j]&7))<=1){
  for(int p=0;p<2;p++){
   unsigned a=(sqs[i]^flip[p])-8+((colors[i]^p)*48),v=(sqs[j]^flip[p])-8+((colors[j]^p)*48);
   unsigned hi=a>v?a:v,lo=a<v?a:v;
   assert(f->npp[p]<128 && hi<96);f->pp[p][f->npp[p]++]=(uint16_t)(hi*(hi-1)/2+lo);
  }
 }
}

typedef struct {const uint32_t *pa,*ps,*aa,*as;int npa,nps,naa,nas;} RowOps;
static void rows_scalar(int16_t *dst,const int16_t *src,const RowOps *o){
 if(dst!=src)memcpy(dst,src,WIDTH*sizeof(int16_t));
 for(int r=0;r<o->npa;r++)for(int j=0;j<WIDTH;j++)dst[j]=wrap(dst[j]+pw[(size_t)o->pa[r]*WIDTH+j]);
 for(int r=0;r<o->nps;r++)for(int j=0;j<WIDTH;j++)dst[j]=wrap(dst[j]-pw[(size_t)o->ps[r]*WIDTH+j]);
 for(int r=0;r<o->naa;r++)for(int j=0;j<WIDTH;j++)dst[j]=wrap(dst[j]+aw[(size_t)o->aa[r]*WIDTH+j]);
 for(int r=0;r<o->nas;r++)for(int j=0;j<WIDTH;j++)dst[j]=wrap(dst[j]-aw[(size_t)o->as[r]*WIDTH+j]);
}
/* Pull the next tile of every touched weight row toward L1 while the current tile is summed. */
static inline void pf_rows(const char *base,size_t rowbytes,const uint32_t *ids,int n,size_t off,size_t span){
 for(int r=0;r<n;r++){const char *q=base+(size_t)ids[r]*rowbytes+off;for(size_t k=0;k<span;k+=64)__builtin_prefetch(q+k,0,3);}
}
static inline void pf_all(const RowOps *o,int tile,int elems){
 size_t ps=(size_t)elems*2,as=(size_t)elems;
 pf_rows((const char*)pw,WIDTH*2,o->pa,o->npa,(size_t)tile*2,ps);pf_rows((const char*)pw,WIDTH*2,o->ps,o->nps,(size_t)tile*2,ps);
 pf_rows((const char*)aw,WIDTH,o->aa,o->naa,(size_t)tile,as);pf_rows((const char*)aw,WIDTH,o->as,o->nas,(size_t)tile,as);
}
/* dst = src + sum(psqt adds) - sum(psqt subs) + sum(aux adds) - sum(aux subs), wrapping int16, one pass over the accumulator. */
__attribute__((target("avx2")))
static void rows_avx2(int16_t *dst,const int16_t *src,const RowOps *o){
#ifndef RV_NO_PREFETCH
 pf_all(o,0,128);
#endif
 for(int tile=0;tile<WIDTH;tile+=128){
#ifndef RV_NO_PREFETCH
  if(tile+128<WIDTH)pf_all(o,tile+128,128);
#endif
  __m256i v[8];for(int k=0;k<8;k++)v[k]=_mm256_loadu_si256((const void*)(src+tile+k*16));
  for(int r=0;r<o->npa;r++)for(int k=0;k<8;k++)v[k]=_mm256_add_epi16(v[k],_mm256_loadu_si256((const void*)(pw+(size_t)o->pa[r]*WIDTH+tile+k*16)));
  for(int r=0;r<o->nps;r++)for(int k=0;k<8;k++)v[k]=_mm256_sub_epi16(v[k],_mm256_loadu_si256((const void*)(pw+(size_t)o->ps[r]*WIDTH+tile+k*16)));
  for(int r=0;r<o->naa;r++)for(int k=0;k<8;k++)v[k]=_mm256_add_epi16(v[k],_mm256_cvtepi8_epi16(_mm_loadu_si128((const void*)(aw+(size_t)o->aa[r]*WIDTH+tile+k*16))));
  for(int r=0;r<o->nas;r++)for(int k=0;k<8;k++)v[k]=_mm256_sub_epi16(v[k],_mm256_cvtepi8_epi16(_mm_loadu_si128((const void*)(aw+(size_t)o->as[r]*WIDTH+tile+k*16))));
  for(int k=0;k<8;k++)_mm256_store_si256((void*)(dst+tile+k*16),v[k]);
 }
}
__attribute__((target("avx512f,avx512bw")))
static void rows_avx512(int16_t *dst,const int16_t *src,const RowOps *o){
#ifndef RV_NO_PREFETCH
 pf_all(o,0,256);
#endif
 for(int tile=0;tile<WIDTH;tile+=256){
#ifndef RV_NO_PREFETCH
  if(tile+256<WIDTH)pf_all(o,tile+256,256);
#endif
  __m512i v[8];for(int k=0;k<8;k++)v[k]=_mm512_loadu_si512((const void*)(src+tile+k*32));
  for(int r=0;r<o->npa;r++)for(int k=0;k<8;k++)v[k]=_mm512_add_epi16(v[k],_mm512_loadu_si512((const void*)(pw+(size_t)o->pa[r]*WIDTH+tile+k*32)));
  for(int r=0;r<o->nps;r++)for(int k=0;k<8;k++)v[k]=_mm512_sub_epi16(v[k],_mm512_loadu_si512((const void*)(pw+(size_t)o->ps[r]*WIDTH+tile+k*32)));
  for(int r=0;r<o->naa;r++)for(int k=0;k<8;k++)v[k]=_mm512_add_epi16(v[k],_mm512_cvtepi8_epi16(_mm256_loadu_si256((const void*)(aw+(size_t)o->aa[r]*WIDTH+tile+k*32))));
  for(int r=0;r<o->nas;r++)for(int k=0;k<8;k++)v[k]=_mm512_sub_epi16(v[k],_mm512_cvtepi8_epi16(_mm256_loadu_si256((const void*)(aw+(size_t)o->as[r]*WIDTH+tile+k*32))));
  for(int k=0;k<8;k++)_mm512_store_si512((void*)(dst+tile+k*32),v[k]);
 }
}
static void (*rows)(int16_t*,const int16_t*,const RowOps*)=rows_scalar;
static uint64_t square_diff(const uint8_t *x,const uint8_t *y){
 uint64_t m=0;
 for(int i=0;i<4;i++){
  __m128i e=_mm_cmpeq_epi8(_mm_loadu_si128((const void*)(x+i*16)),_mm_loadu_si128((const void*)(y+i*16)));
  m|=(uint64_t)(~(unsigned)_mm_movemask_epi8(e)&0xffffu)<<(i*16);
 }
 return m;
}
static void build_frame(Frame *f,const Board *b,const Frame *old){
 memcpy(f->squares,b->piece_on,64);
 f->men=b->occ_all&~(b->pieces[0][KING]|b->pieces[1][KING]);
 f->pawns=b->pieces[0][PAWN]|b->pieces[1][PAWN];
 for(int p=0;p<2;p++){int king=lsb(b->pieces[p][KING]);f->view[p]=(p?56:0)^((king&7)>=4?7:0);f->bucket[p]=king_bucket(b,p);}
 uint64_t changed=old?square_diff(old->squares,b->piece_on):0;
 /* affected: pieces on changed squares, plus pieces whose attack set touched a changed square before.
  * A slider's ray can only open or close through a square it attacked in the old position, so the
  * old attack sets decide this for every other piece. */
 Bitboard affected=0;bool pawns_changed=true;
 if(old){
  memcpy(f->attf,old->attf,sizeof f->attf);
  affected=changed;
  for(Bitboard l=old->men&~changed;l;l&=l-1){int s=__builtin_ctzll(l);if(old->attf[s]&changed)affected|=1ULL<<s;}
  for(Bitboard l=affected;l;l&=l-1){int s=__builtin_ctzll(l),ap=b->piece_on[s];f->attf[s]=(ap==EMPTY_SQUARE||ap%6==KING)?0:attacks(ap,s,b->occ_all);}
  pawns_changed=((old->pawns|f->pawns)&changed)!=0;
 }else{
  memset(f->attf,0,sizeof f->attf);
  for(Bitboard l=f->men;l;l&=l-1){int s=__builtin_ctzll(l);f->attf[s]=attacks(b->piece_on[s],s,b->occ_all);}
 }
 if(old && !pawns_changed && old->view[0]==f->view[0] && old->view[1]==f->view[1]){memcpy(f->pp,old->pp,sizeof f->pp);memcpy(f->npp,old->npp,sizeof f->npp);}
 else pawn_pairs(b,f);
 for(int p=0;p<2;p++){
  uint32_t padd[CAP],psub[CAP],aadd[CAP],asub[CAP];int npa=0,nps=0,naa=0,nas=0;
  const int16_t *src=bias;
  if(old && old->view[p]==f->view[p] && old->bucket[p]==f->bucket[p]){
   src=old->a[p];
   for(uint64_t left=changed;left;left&=left-1){
    int sq=__builtin_ctzll(left);
    int before=old->squares[sq],after=b->piece_on[sq];
    if(before!=EMPTY_SQUARE){int t=before%6,c=t==KING?0:(before/6^p);psub[nps++]=f->bucket[p]*704+c*384+t*64+(sq^f->view[p]);}
    if(after!=EMPTY_SQUARE){int t=after%6,c=t==KING?0:(after/6^p);padd[npa++]=f->bucket[p]*704+c*384+t*64+(sq^f->view[p]);}
   }
   /* Auxiliary features: only edges of affected attackers (and the pawn pairs, if any pawn moved) can differ. */
   uint32_t olde[CAP],newe[CAP];int no=edge_list(old,affected,p,olde,0),nn=edge_list(f,affected,p,newe,0);
   if(pawns_changed){
    for(int i=0;i<old->npp[p];i++)olde[no++]=old->pp[p][i];
    for(int i=0;i<f->npp[p];i++)newe[nn++]=f->pp[p][i];
   }
   /* Set difference through two 8KB bitsets; both are cleared again below. */
   for(int i=0;i<no;i++){uint32_t id=olde[i];seen_old[id>>6]|=1ULL<<(id&63);}
   for(int i=0;i<nn;i++){uint32_t id=newe[i];seen_new[id>>6]|=1ULL<<(id&63);if(!(seen_old[id>>6]>>(id&63)&1))aadd[naa++]=id;}
   for(int i=0;i<no;i++){uint32_t id=olde[i];if(!(seen_new[id>>6]>>(id&63)&1))asub[nas++]=id;}
   for(int i=0;i<no;i++)seen_old[olde[i]>>6]=0;
   for(int i=0;i<nn;i++)seen_new[newe[i]>>6]=0;
  }else{
   /* Fresh frame or perspective flip: rebuild psqt and auxiliary features from scratch. */
   f->np[p]=psqt_list(b,p,f->ps[p]);
   for(int i=0;i<f->np[p];i++)padd[npa++]=f->ps[p][i];
   naa=edge_list(f,f->men,p,aadd,0);
   for(int i=0;i<f->npp[p];i++){assert(naa<CAP);aadd[naa++]=f->pp[p][i];}
  }
  RowOps ops={padd,psub,aadd,asub,npa,nps,naa,nas};
  rows(f->a[p],src,&ops);
 }
 f->ready=true;
}
static const Frame *sync_frame(const Board *b,Frame *scratch,bool fresh){
 int idx=b->ply-base_ply;
 if(fresh || idx<0 || idx>=FRAMES){build_frame(scratch,b,NULL);return scratch;}
 Frame *f=&frames[idx];if(f->ready)return f;
 const Frame *old=NULL;for(int i=idx-1;i>=0;i--)if(frames[i].ready){old=&frames[i];break;}
 build_frame(f,b,old);return f;
}
static float swish(float x){float g=fminf(fmaxf(x+3.0f,0.0f),6.0f);return (x*g)*(1.0f/6.0f);}
static int finish(const int32_t *s,int bucket){
 float x[32],z[64],h[32],sum[16]={0};
 for(int j=0;j<32;j++)x[j]=swish(fmaf((float)s[j],512.0f/(255.0f*255.0f*64.0f),b1[bucket][j]));
 memcpy(z,b2[bucket],sizeof z);
 for(int i=0;i<32;i++)for(int j=0;j<64;j++)z[j]=fmaf(x[i],w2[bucket][i][j],z[j]);
 for(int i=0;i<32;i++)h[i]=swish(z[i])*z[i+32]+x[i];
 for(int i=0;i<32;i++)sum[i%16]=fmaf(h[i],w3[bucket][i],sum[i%16]);
 float t[8],u[4];for(int i=0;i<8;i++)t[i]=sum[i]+sum[i+8];for(int i=0;i<4;i++)u[i]=t[i]+t[i+4];
 float y=((u[0]+u[2])+(u[1]+u[3]))+b3[bucket];
 return (int)(y*240.0f);
}
static void activate(const Frame *f,const Board *b,uint8_t *x){
 for(int p=0;p<2;p++)for(int i=0;i<512;i++){
  const int16_t *a=f->a[b->side^p];x[p*512+i]=(clip(a[i])*clip(a[i+512]))>>9;
 }
}
/* Eight-lane float head, FMA order follows the inference definition. */
__attribute__((target("avx2,fma")))
static int finish_avx2(const int32_t *s,int bucket){
 _Alignas(64) float x[32],h[32];
 __m256 z[8],zero=_mm256_setzero_ps(),three=_mm256_set1_ps(3.0f),six=_mm256_set1_ps(6.0f),inv=_mm256_set1_ps(1.0f/6.0f);
 __m256 scale=_mm256_set1_ps(512.0f/(255.0f*255.0f*64.0f));
 for(int j=0;j<4;j++){
  __m256 v=_mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_load_si256((void*)(s+j*8))),scale,_mm256_load_ps(b1[bucket]+j*8));
  __m256 g=_mm256_min_ps(_mm256_max_ps(_mm256_add_ps(v,three),zero),six);
  _mm256_store_ps(x+j*8,_mm256_mul_ps(_mm256_mul_ps(v,g),inv));
 }
 for(int j=0;j<8;j++)z[j]=_mm256_load_ps(b2[bucket]+j*8);
 for(int i=0;i<32;i++){
  __m256 v=_mm256_set1_ps(x[i]);
  for(int j=0;j<8;j++)z[j]=_mm256_fmadd_ps(v,_mm256_load_ps(w2[bucket][i]+j*8),z[j]);
 }
 for(int j=0;j<4;j++){
  __m256 g=_mm256_min_ps(_mm256_max_ps(_mm256_add_ps(z[j],three),zero),six);
  __m256 sw=_mm256_mul_ps(_mm256_mul_ps(z[j],g),inv);
  _mm256_store_ps(h+j*8,_mm256_add_ps(_mm256_mul_ps(sw,z[j+4]),_mm256_load_ps(x+j*8)));
 }
 __m256 a=_mm256_mul_ps(_mm256_load_ps(h),_mm256_load_ps(w3[bucket]));
 __m256 b=_mm256_mul_ps(_mm256_load_ps(h+8),_mm256_load_ps(w3[bucket]+8));
 a=_mm256_fmadd_ps(_mm256_load_ps(h+16),_mm256_load_ps(w3[bucket]+16),a);
 b=_mm256_fmadd_ps(_mm256_load_ps(h+24),_mm256_load_ps(w3[bucket]+24),b);
 __m256 total=_mm256_add_ps(a,b);
 __m128 t=_mm_add_ps(_mm256_castps256_ps128(total),_mm256_extractf128_ps(total,1));
 t=_mm_add_ps(t,_mm_movehl_ps(t,t));
 t=_mm_add_ss(t,_mm_shuffle_ps(t,t,1));
 return (int)((_mm_cvtss_f32(t)+b3[bucket])*240.0f);
}
__attribute__((target("avx2")))
static void activate_avx2(const Frame *f,const Board *b,uint8_t *x){
 __m256i zero=_mm256_setzero_si256(),cap=_mm256_set1_epi16(255);
 for(int p=0;p<2;p++){
  const int16_t *a=f->a[b->side^p];
  for(int i=0;i<512;i+=32){
   __m256i u=_mm256_min_epi16(_mm256_max_epi16(_mm256_load_si256((void*)(a+i)),zero),cap);
   __m256i v=_mm256_min_epi16(_mm256_max_epi16(_mm256_load_si256((void*)(a+i+512)),zero),cap);
   __m256i q=_mm256_srli_epi16(_mm256_mullo_epi16(u,v),9);
   u=_mm256_min_epi16(_mm256_max_epi16(_mm256_load_si256((void*)(a+i+16)),zero),cap);
   v=_mm256_min_epi16(_mm256_max_epi16(_mm256_load_si256((void*)(a+i+528)),zero),cap);
   __m256i r=_mm256_srli_epi16(_mm256_mullo_epi16(u,v),9);
   __m256i packed=_mm256_permute4x64_epi64(_mm256_packus_epi16(q,r),0xd8);
   _mm256_storeu_si256((void*)(x+p*512+i),packed);
  }
 }
}
static int forward_scalar(const Frame *f,const Board *b){
 uint8_t x[1024];activate(f,b,x);int32_t s[32]={0};int bucket=(popcount(b->occ_all)-2)/4;
 for(int i=0;i<1024;i++)for(int j=0;j<32;j++)s[j]+=x[i]*w1[bucket][(i/4)*128+j*4+i%4];
 return finish(s,bucket);
}
/* Indices of the 4-byte input groups with any nonzero byte, in ascending order. */
__attribute__((target("avx2")))
static inline int nonzero_groups(const uint8_t *x,uint16_t *idx){
 __m256i zero=_mm256_setzero_si256();int n=0;
 for(int c=0;c<32;c++){
  unsigned z=(unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_load_si256((const void*)(x+c*32)),zero)));
  for(unsigned nz=~z&0xffu;nz;nz&=nz-1)idx[n++]=(uint16_t)(c*8+__builtin_ctz(nz));
 }
 return n;
}
__attribute__((target("avx2,fma")))
static int forward_avx2(const Frame *f,const Board *b){
 _Alignas(32) uint8_t x[1024];activate_avx2(f,b,x);int bucket=(popcount(b->occ_all)-2)/4;
 __m256i s[4];for(int j=0;j<4;j++)s[j]=_mm256_setzero_si256();__m256i one=_mm256_set1_epi16(1);
 uint16_t idx[256];int ng=nonzero_groups(x,idx);
 for(int gi=0;gi<ng;gi++){
  int g=idx[gi];uint32_t bits;memcpy(&bits,x+g*4,4);
  __m256i input=_mm256_set1_epi32(bits);
  for(int j=0;j<4;j++){
   __m256i weight=_mm256_load_si256((void*)(w1[bucket]+g*128+j*32));
   s[j]=_mm256_add_epi32(s[j],_mm256_madd_epi16(_mm256_maddubs_epi16(input,weight),one));
  }
 }
 _Alignas(64) int32_t sums[32];for(int j=0;j<4;j++)_mm256_store_si256((void*)(sums+j*8),s[j]);
 return finish_avx2(sums,bucket);
}

__attribute__((target("avx512f,avx512bw,avx512vnni,avx2,fma")))
static int forward_vnni(const Frame *f,const Board *b){
 _Alignas(32) uint8_t x[1024];activate_avx2(f,b,x);int bucket=(popcount(b->occ_all)-2)/4;
 __m512i lo=_mm512_setzero_si512(),hi=_mm512_setzero_si512();
 uint16_t idx[256];int ng=nonzero_groups(x,idx);
 for(int gi=0;gi<ng;gi++){
  int g=idx[gi];uint32_t bits;memcpy(&bits,x+g*4,4);
  __m512i input=_mm512_set1_epi32(bits);
  lo=_mm512_dpbusd_epi32(lo,input,_mm512_load_si512((void*)(w1[bucket]+g*128)));
  hi=_mm512_dpbusd_epi32(hi,input,_mm512_load_si512((void*)(w1[bucket]+g*128+64)));
 }
 _Alignas(64) int32_t sums[32];_mm512_store_si512(sums,lo);_mm512_store_si512(sums+16,hi);
 return finish_avx2(sums,bucket);
}
static int (*forward)(const Frame*,const Board*)=forward_scalar;
bool net_load(const unsigned char *bytes,size_t count){
 if(count!=NET_BYTES)return false;
 unsigned char *fresh=ravager_aligned_alloc(64,(count+63)&~(size_t)63);if(!fresh)return false;memcpy(fresh,bytes,count);
 /* f32 parameters must be finite before installing anything. */
 for(size_t i=L1B_OFFSET;i<count;i+=4){float v;memcpy(&v,fresh+i,4);if(!isfinite(v)){ravager_aligned_free(fresh);return false;}}
 for(int bucket=0;bucket<8;bucket++){
  for(int i=0;i<1024;i++)for(int j=0;j<32;j++)w1[bucket][i/4*128+j*4+i%4]=(int8_t)fresh[L1_OFFSET+(i*8+bucket)*32+j];
  memcpy(b1[bucket],fresh+L1B_OFFSET+bucket*32*4,32*4);
  for(int i=0;i<32;i++)memcpy(w2[bucket][i],fresh+L2_OFFSET+(i*8+bucket)*64*4,64*4);
  memcpy(b2[bucket],fresh+L2B_OFFSET+bucket*64*4,64*4);
  for(int i=0;i<32;i++)memcpy(&w3[bucket][i],fresh+L3_OFFSET+(i*8+bucket)*4,4);
  memcpy(&b3[bucket],fresh+L3B_OFFSET+bucket*4,4);
 }
 ravager_aligned_free(data);data=fresh;aw=(void*)data;pw=(void*)(data+AUX_BYTES);bias=(void*)(data+BIAS_OFFSET);
 feature_tables();rows=rows_scalar;forward=forward_scalar;net_tier="scalar";
 const char *force=getenv("RAVAGER_NNUE_TIER");
 if((!force || strcmp(force,"scalar")) && __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")){rows=rows_avx2;forward=forward_avx2;net_tier="avx2";}
 if((!force || !strcmp(force,"avx512")) && __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")){rows=rows_avx512;net_tier="avx512";if(__builtin_cpu_supports("avx512vnni")){forward=forward_vnni;net_tier="avx512-vnni";}}
 for(int i=0;i<FRAMES;i++)frames[i].ready=false;
 return true;
}
void net_prepare(const Board *b){base_ply=b->ply;for(int i=0;i<FRAMES;i++)frames[i].ready=false;build_frame(&frames[0],b,NULL);}
void net_push(const Board *b){int i=b->ply-base_ply+1;if(i>0 && i<FRAMES)frames[i].ready=false;}
int net_eval(const Board *b,bool fresh){Frame scratch;return forward(sync_frame(b,&scratch,fresh),b);}
int net_eval_scalar(const Board *b){Frame scratch;build_frame(&scratch,b,NULL);return forward_scalar(&scratch,b);}
bool net_check(const Board *b){Frame a;build_frame(&a,b,NULL);Frame temp;const Frame *lazy=sync_frame(b,&temp,false);return !memcmp(a.a,lazy->a,sizeof a.a) && forward_scalar(&a,b)==forward(lazy,b);}
