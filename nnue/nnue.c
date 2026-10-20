/* nnue.c — installs the embedded or an external network. */
#include "nnue.h"
#include "inference.h"
#ifdef EVALFILE
#include "incbin.h"
INCBIN(RavagerNet, EVALFILE);
#endif

bool nnue_loaded = false, nnue_enabled = true;
const char *nnue_tier = "scalar";

static bool install(const unsigned char *buf, size_t n) {
    if (n != NET_BYTES || !net_load(buf, n)) return false;
    nnue_loaded = true;
    nnue_tier = net_tier;
    return true;
}

bool nnue_load_embedded(void) {
#ifdef EVALFILE
    return install(gRavagerNetData, gRavagerNetSize);
#else
    return false;
#endif
}

bool nnue_load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return false; }
    long n = ftell(f);
    rewind(f);
    if (n != (long)NET_BYTES) { fclose(f); return false; }
    unsigned char *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return false; }
    bool ok = fread(buf, 1, (size_t)n, f) == (size_t)n && !ferror(f);
    fclose(f);
    if (ok) ok = install(buf, (size_t)n);
    free(buf);
    return ok;
}

void nnue_prepare_search(const Board *b) { if (nnue_loaded) net_prepare(b); }
void nnue_push_move(const Board *b, Move m) { (void)m; if (nnue_loaded) net_push(b); }
void nnue_push_null(const Board *b) { if (nnue_loaded) net_push(b); }
int32_t nnue_raw_output(const Board *b, bool fresh) { return nnue_loaded ? net_eval(b, fresh) : 0; }

int nnue_evaluate_board(const Board *b) {
    if (!nnue_loaded || !nnue_enabled) return evaluate((Board *)b);
    int raw = nnue_raw_output(b, true);
    int phase = b->game_phase > 24 ? 24 : b->game_phase;
    int v = raw * (128 + phase) / 152;
    return v < -30076 ? -30076 : v > 30076 ? 30076 : v;
}
