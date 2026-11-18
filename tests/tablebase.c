/* Verify adapter arguments without requiring installed tablebase files. */
#include "../src/tb_syzygy.c"
#include <assert.h>

int TB_LARGEST = 7;
static unsigned observed_ep;
static bool observed_turn;

bool tb_init(const char *path) { (void)path; return true; }

unsigned tb_probe_wdl(uint64_t white, uint64_t black, uint64_t kings, uint64_t queens,
                      uint64_t rooks, uint64_t bishops, uint64_t knights, uint64_t pawns,
                      unsigned ep, bool turn) {
    observed_ep = ep;
    observed_turn = turn;
    return TB_DRAW;
}

unsigned tb_probe_root(uint64_t white, uint64_t black, uint64_t kings, uint64_t queens,
                       uint64_t rooks, uint64_t bishops, uint64_t knights, uint64_t pawns,
                       unsigned rule50, unsigned ep, bool turn, unsigned *results) {
    observed_ep = ep;
    observed_turn = turn;
    return TB_RESULT_FAILED;
}

int main(void) {
    board_init_all();
    tb_ok = true;
    const char *fens[] = {
        "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1",
        "4k3/8/8/8/3Pp3/8/8/4K3 b - d3 0 1",
        "4k3/8/8/8/8/8/8/4K3 w - - 0 1"
    };
    const unsigned expected[] = {43, 19, 0};
    for (int i = 0; i < 3; i++) {
        Board b;
        int wdl;
        assert(parse_fen(&b, fens[i]));
        observed_ep = 99;
        assert(syzygy_probe_wdl(&b, &wdl) && wdl == 0);
        assert(observed_ep == expected[i] && observed_turn == (b.side == WHITE));
        observed_ep = 99;
        assert(syzygy_probe_root(&b) == NO_MOVE);
        assert(observed_ep == expected[i] && observed_turn == (b.side == WHITE));
    }
    puts("Tablebase adapter regressions passed");
    return 0;
}
