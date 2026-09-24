/*
    Minimal, config-agnostic smoke test. Unlike test_unit.c (which assumes
    a "normal-shaped" pool - distinct A<B<C sizes and enough blocks per
    class for several concurrent allocations), this file makes no
    assumptions beyond what every valid configuration guarantees: each
    pool has at least 1 block. Used in run_tests.sh for extreme edge
    configs (e.g. a minimum-viable 1-block-per-class pool) where
    test_unit.c's boundary tests don't have room to run.
*/
#include "eccles_rtmem.h"
#include <assert.h>
#include <stdio.h>

int main(void){
    assert(eccles_rtmem_init() == true);

    eccles_rt_stats_t s = eccles_rt_get_stats();
    assert((unsigned)s.usedA + s.freeA == ECCLES_RT_POOL_A_COUNT);
    assert((unsigned)s.usedB + s.freeB == ECCLES_RT_POOL_B_COUNT);
    assert((unsigned)s.usedC + s.freeC == ECCLES_RT_POOL_C_COUNT);
    assert(s.usedA == 0 && s.usedB == 0 && s.usedC == 0);

    /* smallest possible allocation must succeed in a fresh pool */
    uint8_t *p = eccles_rt_malloc(1);
    assert(p != NULL);
    assert(eccles_rt_free(p) == ECCLES_RT_FREE_OK);

    /* zero-byte request always NULL, regardless of pool shape */
    assert(eccles_rt_malloc(0) == NULL);

    /* an exact-C-block request must succeed in a fresh pool */
    uint8_t *c = eccles_rt_malloc(ECCLES_RT_BLOCK_C_SIZE);
    assert(c != NULL);

    /* double free rejected */
    assert(eccles_rt_free(c) == ECCLES_RT_FREE_OK);
    assert(eccles_rt_free(c) == ECCLES_RT_FREE_DOUBLE_FREE);

    /* out-of-range pointer rejected without touching real state */
    int local = 0;
    assert(eccles_rt_free((uint8_t*)&local) == ECCLES_RT_FREE_OUT_OF_RANGE);

    /* NULL free is a no-op */
    assert(eccles_rt_free(NULL) == ECCLES_RT_FREE_NULL);

    s = eccles_rt_get_stats();
    assert(s.usedA == 0 && s.usedB == 0 && s.usedC == 0);

    printf("PASS smoke test (config-agnostic)\n");
    return 0;
}
