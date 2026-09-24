/*
    Host-side unit tests for eccles_rtmem, built with the default (20 KB,
    64/128/256-byte blocks, 80/40/40 counts) configuration. Run with:

        gcc -std=c99 -Wall -Wextra -I.. ../eccles_rtmem.c test_unit.c -o test_unit && ./test_unit

    Every check is a plain assert() - a failure aborts immediately with a
    file/line pointing at the broken invariant, which is what you want
    from a test run: loud and specific, not a summary you have to dig
    through.
*/
#include "eccles_rtmem.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define A_SIZE ECCLES_RT_BLOCK_A_SIZE
#define B_SIZE ECCLES_RT_BLOCK_B_SIZE
#define C_SIZE ECCLES_RT_BLOCK_C_SIZE
#define A_COUNT ECCLES_RT_POOL_A_COUNT
#define B_COUNT ECCLES_RT_POOL_B_COUNT
#define C_COUNT ECCLES_RT_POOL_C_COUNT

static void check_counts(uint8_t usedA, uint8_t usedB, uint8_t usedC, const char *label){
    eccles_rt_stats_t s = eccles_rt_get_stats();
    if(s.usedA != usedA || s.usedB != usedB || s.usedC != usedC){
        printf("FAIL [%s]: expected used A=%u B=%u C=%u, got A=%u B=%u C=%u\n",
            label, usedA, usedB, usedC, s.usedA, s.usedB, s.usedC);
        assert(0);
    }
    /* used+free must always equal the pool's total block count */
    assert((unsigned)s.usedA + s.freeA == A_COUNT);
    assert((unsigned)s.usedB + s.freeB == B_COUNT);
    assert((unsigned)s.usedC + s.freeC == C_COUNT);
}

static void test_malloc_zero(void){
    uint8_t *p = eccles_rt_malloc(0);
    assert(p == NULL);
    check_counts(0, 0, 0, "malloc(0)");
    printf("PASS malloc(0) -> NULL, no pool touched\n");
}

static void test_smallest_allocation(void){
    uint8_t *p = eccles_rt_malloc(1);
    assert(p != NULL);
    check_counts(1, 0, 0, "smallest allocation");
    assert(eccles_rt_free(p) == ECCLES_RT_FREE_OK);
    check_counts(0, 0, 0, "smallest allocation freed");
    printf("PASS smallest allocation (1 byte) lands in pool A\n");
}

static void test_class_boundaries(void){
    uint8_t *a  = eccles_rt_malloc(A_SIZE);       /* exactly A -> pool A */
    uint8_t *a1 = eccles_rt_malloc(A_SIZE + 1);    /* just over A -> pool B */
    uint8_t *b  = eccles_rt_malloc(B_SIZE);        /* exactly B -> pool B */
    uint8_t *b1 = eccles_rt_malloc(B_SIZE + 1);    /* just over B -> pool C */
    uint8_t *c  = eccles_rt_malloc(C_SIZE);        /* exactly C -> pool C, one block */
    assert(a && a1 && b && b1 && c);
    check_counts(1, 2, 2, "class boundaries");
    assert(eccles_rt_free(a)  == ECCLES_RT_FREE_OK);
    assert(eccles_rt_free(a1) == ECCLES_RT_FREE_OK);
    assert(eccles_rt_free(b)  == ECCLES_RT_FREE_OK);
    assert(eccles_rt_free(b1) == ECCLES_RT_FREE_OK);
    assert(eccles_rt_free(c)  == ECCLES_RT_FREE_OK);
    check_counts(0, 0, 0, "class boundaries freed");
    printf("PASS A/B/C class-boundary routing (exact size vs size+1)\n");
}

static void test_large_multiblock(void){
    /* just over one C block -> a 2-block run, still pool C only */
    uint8_t *p = eccles_rt_malloc(C_SIZE + 1);
    assert(p != NULL);
    check_counts(0, 0, 2, "2-block run");

    /* free the middle of that run -> rejected as ECCLES_RT_FREE_MID_RUN,
       registry must be untouched by a rejected free */
    eccles_rt_free_status_t st = eccles_rt_free(p + C_SIZE);
    assert(st == ECCLES_RT_FREE_MID_RUN);
    check_counts(0, 0, 2, "mid-run free correctly rejected, nothing changed");

    assert(eccles_rt_free(p) == ECCLES_RT_FREE_OK);
    check_counts(0, 0, 0, "2-block run freed from its start");

    /* a genuinely large request: 3 full C blocks */
    uint8_t *big = eccles_rt_malloc((size_t)C_SIZE * 3);
    assert(big != NULL);
    check_counts(0, 0, 3, "3-block run");
    assert(eccles_rt_free(big) == ECCLES_RT_FREE_OK);
    check_counts(0, 0, 0, "3-block run freed");

    printf("PASS large allocation -> single-pool multi-block run, mid-run free rejected\n");
}

static void test_double_free(void){
    uint8_t *p = eccles_rt_malloc(10);
    assert(p != NULL);
    assert(eccles_rt_free(p) == ECCLES_RT_FREE_OK);
    assert(eccles_rt_free(p) == ECCLES_RT_FREE_DOUBLE_FREE); /* second free of the same pointer */
    check_counts(0, 0, 0, "double free left state clean");
    printf("PASS double free rejected, first free's result unaffected\n");
}

static void test_invalid_and_misaligned_pointers(void){
    int local_stack_var = 0;
    eccles_rt_free_status_t st;

    /* a pointer nowhere near our pool at all */
    st = eccles_rt_free((uint8_t*)&local_stack_var);
    assert(st == ECCLES_RT_FREE_OUT_OF_RANGE);

    /* a pointer inside the pool, but not on a block boundary */
    uint8_t *p = eccles_rt_malloc(1);
    assert(p != NULL);
    if(A_SIZE > 1){
        st = eccles_rt_free(p + 1);
        assert(st == ECCLES_RT_FREE_MISALIGNED);
    }
    assert(eccles_rt_free(p) == ECCLES_RT_FREE_OK); /* the real, aligned pointer still frees fine */

    check_counts(0, 0, 0, "invalid/misaligned pointers left state clean");
    printf("PASS out-of-range and misaligned pointers rejected without corrupting state\n");
}

static void test_null_free(void){
    assert(eccles_rt_free(NULL) == ECCLES_RT_FREE_NULL);
    printf("PASS free(NULL) is a harmless no-op\n");
}

static void test_pool_exhaustion_and_reuse(void){
    uint8_t *bufs[A_COUNT];
    unsigned i;
    for(i = 0; i < A_COUNT; i++){
        bufs[i] = eccles_rt_malloc(1);
        assert(bufs[i] != NULL);
    }
    check_counts(A_COUNT, 0, 0, "pool A fully exhausted");

    /* one more request into the exhausted pool must fail cleanly */
    assert(eccles_rt_malloc(1) == NULL);

    /* free exactly one slot, verify exactly one more allocation succeeds
       and lands in that same freed slot (there's nowhere else free) */
    uint8_t *freedSlot = bufs[5];
    assert(eccles_rt_free(freedSlot) == ECCLES_RT_FREE_OK);
    check_counts(A_COUNT - 1, 0, 0, "one slot freed");

    uint8_t *reused = eccles_rt_malloc(1);
    assert(reused == freedSlot);
    check_counts(A_COUNT, 0, 0, "freed slot reused by cursor search");
    bufs[5] = reused;

    for(i = 0; i < A_COUNT; i++){
        assert(eccles_rt_free(bufs[i]) == ECCLES_RT_FREE_OK);
    }
    check_counts(0, 0, 0, "pool A fully freed");
    printf("PASS pool exhaustion, clean failure at capacity, freed-slot reuse\n");
}

static void test_fragmentation(void){
    /* to prove C's fragmentation alone causes failure (not just "C failed,
       fell through to B or A which still had room"), fill A and B to
       capacity too, so the multi-block fallback chain has nowhere else
       to go once C has no runnable gap left */
    uint8_t *aBufs[A_COUNT];
    uint8_t *bBufs[B_COUNT];
    uint8_t *cBufs[C_COUNT];
    unsigned i;

    for(i = 0; i < A_COUNT; i++){ aBufs[i] = eccles_rt_malloc(1); assert(aBufs[i]); }
    for(i = 0; i < B_COUNT; i++){ bBufs[i] = eccles_rt_malloc(A_SIZE + 1); assert(bBufs[i]); }
    for(i = 0; i < C_COUNT; i++){ cBufs[i] = eccles_rt_malloc(C_SIZE); assert(cBufs[i]); }
    check_counts(A_COUNT, B_COUNT, C_COUNT, "A, B, and C all fully exhausted");

    /* free exactly ONE block in C. With every other block in the entire
       allocator still used, that single free block is guaranteed to have
       no free neighbour - whichever absolute registry slot it actually
       landed on (which depends on the pool's current cursor position, not
       under this test's control) - so no 2-block run can exist anywhere.
       This is deterministic regardless of prior cursor state, unlike a
       "free every other" pattern, whose apparent isolation can depend on
       the pool's block count's parity relative to where its cursor
       happened to be sitting - a test-fragility that's worth naming
       explicitly rather than silently avoiding. */
    assert(eccles_rt_free(cBufs[0]) == ECCLES_RT_FREE_OK);
    check_counts(A_COUNT, B_COUNT, C_COUNT - 1, "exactly one C block freed - A/B still full");

    uint8_t *twoBlock = eccles_rt_malloc((size_t)C_SIZE + 1);
    assert(twoBlock == NULL);

    /* that single free slot is still enough for a same-size request */
    uint8_t *reused = eccles_rt_malloc(C_SIZE);
    assert(reused != NULL);
    check_counts(A_COUNT, B_COUNT, C_COUNT, "single free slot reused");

    /* clean up */
    for(i = 0; i < A_COUNT; i++) assert(eccles_rt_free(aBufs[i]) == ECCLES_RT_FREE_OK);
    for(i = 0; i < B_COUNT; i++) assert(eccles_rt_free(bBufs[i]) == ECCLES_RT_FREE_OK);
    for(i = 1; i < C_COUNT; i++) assert(eccles_rt_free(cBufs[i]) == ECCLES_RT_FREE_OK);
    assert(eccles_rt_free(reused) == ECCLES_RT_FREE_OK);
    check_counts(0, 0, 0, "fragmentation test cleaned up");
    printf("PASS fragmented pool (with no fallback room) correctly refuses a multi-block run it can't satisfy\n");
}

static void test_repeated_init(void){
    uint8_t *p = eccles_rt_malloc(10);
    assert(p != NULL);
    eccles_rt_stats_t before = eccles_rt_get_stats();

    /* calling init again must be a safe no-op: it must NOT reset cursors,
       reallocate the heap pool, or otherwise disturb the buffer already
       handed out above */
    assert(eccles_rtmem_init() == true);

    eccles_rt_stats_t after = eccles_rt_get_stats();
    assert(memcmp(&before, &after, sizeof before) == 0);
    assert(eccles_rt_free(p) == ECCLES_RT_FREE_OK); /* still a valid, freeable pointer */
    check_counts(0, 0, 0, "repeated init left everything intact");
    printf("PASS eccles_rtmem_init() is idempotent\n");
}

int main(void){
    assert(eccles_rtmem_init() == true);
    check_counts(0, 0, 0, "startup");

    test_malloc_zero();
    test_smallest_allocation();
    test_class_boundaries();
    test_large_multiblock();
    test_double_free();
    test_invalid_and_misaligned_pointers();
    test_null_free();
    test_pool_exhaustion_and_reuse();
    test_fragmentation();
    test_repeated_init();

    printf("\nALL UNIT TESTS PASSED\n");
    return 0;
}
