#include "moqt_feasibility.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    moqt_rate r = {0};
    assert(moqt_rate_value(&r) == 64000);
    const size_t sizes[] = {100000, 30000, 10000};
    assert(moqt_choose_representation(sizes, 3, 16000, 0, 2000000) == 2);
    assert(moqt_choose_representation(sizes, 3, 64000, 0, 2000000) == 1);
    assert(moqt_choose_representation(sizes, 3, 100000, 0, 2000000) == 0);
    assert(moqt_choose_representation(sizes, 3, 1000, 0, 2000000) == -1);
    assert(!moqt_fits(10000, 0, 16000, 2000000, 2000000));
    assert(!moqt_fits(10000, 0, 0, 0, UINT64_MAX));
    assert(!moqt_fits(SIZE_MAX, 0, 64000, 0, UINT64_MAX));
    assert(!moqt_fits(10000, SIZE_MAX, 64000, 0, UINT64_MAX));
    assert(moqt_fits(10000, 0, 16000, 0, 2000000));
    assert(!moqt_fits(10000, 30000, 16000, 0, 2000000));
    moqt_rate_observe(&r, 100000, 2000000);
    assert(moqt_rate_value(&r) == 50000);
    moqt_rate_observe(&r, 10000, 2000000); // timeout partial ACKs count
    assert(moqt_rate_value(&r) == 5000);
    moqt_rate_observe(&r, 100000, 2000000);
    assert(moqt_rate_value(&r) == 16250);
    moqt_rate_observe(&r, 0, 2000000);
    assert(moqt_rate_value(&r) == 1000);
    puts("PASS: feasibility bounds, backlog, representation choice, asymmetric adaptation");
}
