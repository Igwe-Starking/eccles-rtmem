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
#if defined(ECCLES_RT_MALLOC_MIN_WASTE)
    /* with MIN_WASTE, a request just over one C block doesn't necessarily
       land in C: for this default 64/128/256 config, 257 bytes needs
       ceil(257/64)=5 A-blocks (320B, 63 wasted) vs 3 B-blocks (384B, 127
       wasted) vs 2 C-blocks (512B, 255 wasted) - A wins on raw waste */
    uint8_t *p = eccles_rt_malloc(C_SIZE + 1);
    assert(p != NULL);
    check_counts(5, 0, 0, "5-block run in pool A (MIN_WASTE: lowest waste wins)");

    eccles_rt_status_t st = eccles_rt_free(p + A_SIZE); /* mid-run, not the run's start */
    assert(st == ECCLES_RT_FREE_MID_RUN);
    check_counts(5, 0, 0, "mid-run free correctly rejected, nothing changed");

    assert(eccles_rt_free(p) == ECCLES_RT_FREE_OK);
    check_counts(0, 0, 0, "5-block run freed from its start");
#else
    /* just over one C block -> a 2-block run, still pool C only */
    uint8_t *p = eccles_rt_malloc(C_SIZE + 1);
    assert(p != NULL);
    check_counts(0, 0, 2, "2-block run");

    /* free the middle of that run -> rejected as ECCLES_RT_FREE_MID_RUN,
       registry must be untouched by a rejected free */
    eccles_rt_status_t st = eccles_rt_free(p + C_SIZE);
    assert(st == ECCLES_RT_FREE_MID_RUN);
    check_counts(0, 0, 2, "mid-run free correctly rejected, nothing changed");

    assert(eccles_rt_free(p) == ECCLES_RT_FREE_OK);
    check_counts(0, 0, 0, "2-block run freed from its start");
#endif

    /* a genuinely large request: 3 full C blocks. Zero waste, so both
       strategies agree C is the right (indeed only sane) choice here. */
    uint8_t *big = eccles_rt_malloc((size_t)C_SIZE * 3);
    assert(big != NULL);
    check_counts(0, 0, 3, "3-block run, zero-waste case picks C under either strategy");
    assert(eccles_rt_free(big) == ECCLES_RT_FREE_OK);
    check_counts(0, 0, 0, "3-block run freed");

    printf("PASS large allocation -> single-pool multi-block run, mid-run free rejected\n");
}

static void test_double_free(void){
    uint8_t *p = eccles_rt_malloc(10);
    assert(p != NULL);
    assert(eccles_rt_free(p) == ECCLES_RT_FREE_OK);
    assert(eccles_rt_free(p) == ECCLES_RT_FREE_ALREADY_FREE); /* second free of the same pointer */
    check_counts(0, 0, 0, "double free left state clean");
    printf("PASS double free rejected, first free's result unaffected\n");
}

static void test_invalid_and_misaligned_pointers(void){
    int local_stack_var = 0;
    eccles_rt_status_t st;

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

static void test_small_allocation_escalation(void){
    /* fill pool A completely, then verify a small request escalates to a
       bigger class instead of failing outright - this is the fallback
       feature: a request that fits pool A shouldn't fail just because A
       specifically is full while B or C has room */
    uint8_t *aBufs[A_COUNT];
    unsigned i;
    for(i = 0; i < A_COUNT; i++){ aBufs[i] = eccles_rt_malloc(1); assert(aBufs[i]); }
    check_counts(A_COUNT, 0, 0, "pool A exhausted");

    uint8_t *escalated = eccles_rt_malloc(1); /* still <= A_SIZE, but A has no room */
    assert(escalated != NULL);
    eccles_rt_stats_t s = eccles_rt_get_stats();
    assert(s.usedA == A_COUNT);
    assert(s.usedB == 1 || s.usedC >= 1); /* landed in B (or C, if B was also exhausted) */
    assert(eccles_rt_free(escalated) == ECCLES_RT_FREE_OK);

    for(i = 0; i < A_COUNT; i++) assert(eccles_rt_free(aBufs[i]) == ECCLES_RT_FREE_OK);
    check_counts(0, 0, 0, "escalation test cleaned up");

    /* now fill A and B, verify a <=B-sized request escalates to C */
    uint8_t *bBufs[B_COUNT];
    for(i = 0; i < A_COUNT; i++){ aBufs[i] = eccles_rt_malloc(1); assert(aBufs[i]); }
    for(i = 0; i < B_COUNT; i++){ bBufs[i] = eccles_rt_malloc(A_SIZE + 1); assert(bBufs[i]); }
    check_counts(A_COUNT, B_COUNT, 0, "pools A and B both exhausted");

    uint8_t *escalatedToC = eccles_rt_malloc(A_SIZE + 1); /* <= B_SIZE, but B has no room */
    assert(escalatedToC != NULL);
    check_counts(A_COUNT, B_COUNT, 1, "escalated past full A and B into C");
    assert(eccles_rt_free(escalatedToC) == ECCLES_RT_FREE_OK);

    for(i = 0; i < A_COUNT; i++) assert(eccles_rt_free(aBufs[i]) == ECCLES_RT_FREE_OK);
    for(i = 0; i < B_COUNT; i++) assert(eccles_rt_free(bBufs[i]) == ECCLES_RT_FREE_OK);
    check_counts(0, 0, 0, "A->B->C escalation test cleaned up");
    printf("PASS small allocations escalate to a larger class when their own class is full\n");
}

static void test_full_exhaustion_returns_null(void){
    /* the ONLY case a small request should genuinely fail: every class
       big enough to hold it is also full, so there's nowhere left to
       escalate to */
    uint8_t *aBufs[A_COUNT];
    uint8_t *bBufs[B_COUNT];
    uint8_t *cBufs[C_COUNT];
    unsigned i;
    for(i = 0; i < A_COUNT; i++){ aBufs[i] = eccles_rt_malloc(1); assert(aBufs[i]); }
    for(i = 0; i < B_COUNT; i++){ bBufs[i] = eccles_rt_malloc(A_SIZE + 1); assert(bBufs[i]); }
    for(i = 0; i < C_COUNT; i++){ cBufs[i] = eccles_rt_malloc(B_SIZE + 1); assert(cBufs[i]); }
    check_counts(A_COUNT, B_COUNT, C_COUNT, "A, B, and C all exhausted");

    assert(eccles_rt_malloc(1) == NULL); /* nothing left to escalate to */

    for(i = 0; i < A_COUNT; i++) assert(eccles_rt_free(aBufs[i]) == ECCLES_RT_FREE_OK);
    for(i = 0; i < B_COUNT; i++) assert(eccles_rt_free(bBufs[i]) == ECCLES_RT_FREE_OK);
    for(i = 0; i < C_COUNT; i++) assert(eccles_rt_free(cBufs[i]) == ECCLES_RT_FREE_OK);
    check_counts(0, 0, 0, "full exhaustion test cleaned up");
    printf("PASS malloc fails cleanly only once every escalation target is also exhausted\n");
}

static void test_pool_exhaustion_and_reuse(void){
    uint8_t *bufs[A_COUNT];
    unsigned i;
    for(i = 0; i < A_COUNT; i++){
        bufs[i] = eccles_rt_malloc(1);
        assert(bufs[i] != NULL);
    }
    check_counts(A_COUNT, 0, 0, "pool A fully exhausted");

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
    printf("PASS pool exhaustion + freed-slot reuse (still prefers the smallest fitting class)\n");
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

static void test_calloc(void){
    /* zero-initialized */
    uint8_t *p = eccles_rt_calloc(10, 4); /* 40 bytes */
    assert(p != NULL);
    for(int i = 0; i < 40; i++) assert(p[i] == 0);
    assert(eccles_rt_free(p) == ECCLES_RT_FREE_OK);

    /* zero count or zero size -> NULL, no pool touched */
    assert(eccles_rt_calloc(0, 4) == NULL);
    assert(eccles_rt_calloc(4, 0) == NULL);
    check_counts(0, 0, 0, "calloc(0,x)/calloc(x,0) touched nothing");

    /* overflow-safe multiply: count*size deliberately wraps size_t */
    assert(eccles_rt_calloc((size_t)-1, 2) == NULL);
    check_counts(0, 0, 0, "calloc overflow rejected cleanly");

    printf("PASS calloc: zero-initialized, rejects zero args and overflow\n");
}

static void test_realloc(void){
    /* realloc(NULL, x) behaves like malloc(x) */
    uint8_t *p = eccles_rt_realloc(NULL, 10);
    assert(p != NULL);
    for(int i = 0; i < 10; i++) p[i] = (uint8_t)(i + 1);

    /* growing within the same already-allocated capacity (A_SIZE) is a
       no-move fast path: same pointer, contents untouched */
    uint8_t *p2 = eccles_rt_realloc(p, A_SIZE);
    assert(p2 == p);
    for(int i = 0; i < 10; i++) assert(p[i] == (uint8_t)(i + 1));

    /* growing past the current class's capacity moves the allocation,
       preserving the original content */
    uint8_t *p3 = eccles_rt_realloc(p2, (size_t)C_SIZE + 1);
    assert(p3 != NULL);
    for(int i = 0; i < 10; i++) assert(p3[i] == (uint8_t)(i + 1));

    /* realloc(ptr, 0) frees ptr and returns NULL */
    uint8_t *p4 = eccles_rt_realloc(p3, 0);
    assert(p4 == NULL);
    check_counts(0, 0, 0, "realloc chain cleaned up");

    /* a failed realloc leaves the original pointer untouched and valid -
       force failure by exhausting every pool first */
    uint8_t *aBufs[A_COUNT], *bBufs[B_COUNT], *cBufs[C_COUNT];
    unsigned i;
    for(i = 0; i < A_COUNT; i++){ aBufs[i] = eccles_rt_malloc(1); assert(aBufs[i]); }
    for(i = 0; i < B_COUNT; i++){ bBufs[i] = eccles_rt_malloc(A_SIZE + 1); assert(bBufs[i]); }
    for(i = 1; i < C_COUNT; i++){ cBufs[i] = eccles_rt_malloc(C_SIZE); assert(cBufs[i]); } /* leave cBufs[0] free for the live allocation below */

    uint8_t *live = eccles_rt_malloc(C_SIZE);
    assert(live != NULL);
    live[0] = 0xAB;
    check_counts(A_COUNT, B_COUNT, C_COUNT, "everything exhausted, `live` holds the last free C slot");

    uint8_t *grown = eccles_rt_realloc(live, (size_t)C_SIZE * 2); /* needs 2 blocks, none free anywhere */
    assert(grown == NULL);
    assert(live[0] == 0xAB); /* `live` is still valid and untouched after the failed realloc */
    assert(eccles_rt_free(live) == ECCLES_RT_FREE_OK);

    for(i = 0; i < A_COUNT; i++) assert(eccles_rt_free(aBufs[i]) == ECCLES_RT_FREE_OK);
    for(i = 0; i < B_COUNT; i++) assert(eccles_rt_free(bBufs[i]) == ECCLES_RT_FREE_OK);
    for(i = 1; i < C_COUNT; i++) assert(eccles_rt_free(cBufs[i]) == ECCLES_RT_FREE_OK);
    check_counts(0, 0, 0, "realloc failure test cleaned up");

    printf("PASS realloc: NULL->malloc, same-capacity fast path, growth preserves content, 0->free, failure leaves original valid\n");
}

static void test_realloc_in_place(void){
    /* geometry-adaptive: some test configs give pool C only 4 blocks */
    enum { G = (C_COUNT < 5) ? C_COUNT : 5 };   /* grow target, in C blocks */
    uint8_t *a, *g, *sh, *sh1, *t;

    /* earlier tests leave the next-fit cursors mid-pool; a run that starts
       at block 1 of a 4-block pool correctly can't grow to 4 blocks without
       crossing the pool boundary, so start from known cursor positions */
    assert(eccles_rt_reset());

    /* GROW into the free blocks right after the run: pointer must not move,
       content must survive, and the pool's used count grows by the delta */
    a = eccles_rt_malloc((size_t)C_SIZE * 2);            /* 2-block run in pool C */
    assert(a != NULL);
    memset(a, 0x5A, (size_t)C_SIZE * 2);
    check_counts(0, 0, 2, "2-block run");

    g = eccles_rt_realloc(a, (size_t)C_SIZE * G);
    assert(g == a);                                       /* extended, not moved */
    for(size_t i = 0; i < (size_t)C_SIZE * 2; i++) assert(g[i] == 0x5A);
    check_counts(0, 0, G, "grown in place");

    /* SHRINK: trailing blocks go straight back to the pool, pointer stays */
    sh = eccles_rt_realloc(g, (size_t)C_SIZE * 2 - 1);
    assert(sh == g);
    check_counts(0, 0, 2, "shrunk in place to 2 blocks");
    sh1 = eccles_rt_realloc(sh, (size_t)C_SIZE + 1);      /* still 2 blocks */
    assert(sh1 == sh);
    check_counts(0, 0, 2, "same block count is a no-op");

    /* the released tail is genuinely reusable, and the shrunk run still
       frees as exactly 2 blocks */
    t = eccles_rt_malloc((size_t)C_SIZE * (G - 2));
    assert(t != NULL);
    check_counts(0, 0, G, "tail reused by a neighbour");
    assert(eccles_rt_free(t)   == ECCLES_RT_FREE_OK);
    assert(eccles_rt_free(sh1) == ECCLES_RT_FREE_OK);
    check_counts(0, 0, 0, "in-place shrink/grow cleaned up");

    /* BLOCKED neighbour: growth must NOT stomp it. Outcome depends on how
       much room the config has elsewhere (move succeeds, or NULL with the
       original intact) - but the neighbour and the old content must survive
       either way. */
    {
        uint8_t *x = eccles_rt_malloc((size_t)C_SIZE * 2);
        uint8_t *y = eccles_rt_malloc(C_SIZE);            /* sits right after x */
        uint8_t *x2;
        assert(x && y);
        memset(x, 0x11, (size_t)C_SIZE * 2);
        memset(y, 0x22, C_SIZE);
        x2 = eccles_rt_realloc(x, (size_t)C_SIZE * 3);
        for(size_t i = 0; i < C_SIZE; i++) assert(y[i] == 0x22);
        if(x2 == NULL){
            for(size_t i = 0; i < (size_t)C_SIZE * 2; i++) assert(x[i] == 0x11); /* original still valid */
            assert(eccles_rt_free(x) == ECCLES_RT_FREE_OK);
        } else {
            assert(x2 != x);                              /* it had to move */
            for(size_t i = 0; i < (size_t)C_SIZE * 2; i++) assert(x2[i] == 0x11);
            assert(eccles_rt_free(x2) == ECCLES_RT_FREE_OK);
        }
        assert(eccles_rt_free(y) == ECCLES_RT_FREE_OK);
        check_counts(0, 0, 0, "blocked-grow cleaned up");
    }

    /* POOL BOUNDARY: with pool C completely full, its last run can't grow
       past the pool end into anything - it either moves elsewhere or fails
       cleanly with the original intact */
    {
        uint8_t *runs[C_COUNT];
        unsigned n = 0, k;
        uint8_t *r, *res;
        while(n < C_COUNT && (r = eccles_rt_malloc(C_SIZE)) != NULL){ runs[n++] = r; }
        assert(n == C_COUNT);
        memset(runs[n - 1], 0x33, C_SIZE);
        res = eccles_rt_realloc(runs[n - 1], (size_t)C_SIZE * 2);
        if(res == NULL){
            assert(runs[n - 1][0] == 0x33);
            assert(eccles_rt_free(runs[n - 1]) == ECCLES_RT_FREE_OK);
        } else {
            assert(res != runs[n - 1]);
            for(size_t i = 0; i < C_SIZE; i++) assert(res[i] == 0x33);
            assert(eccles_rt_free(res) == ECCLES_RT_FREE_OK);
        }
        for(k = 0; k + 1 < n; k++) assert(eccles_rt_free(runs[k]) == ECCLES_RT_FREE_OK);
        check_counts(0, 0, 0, "boundary test cleaned up");
    }

    printf("PASS realloc in place: grow into free neighbours, shrink releases tail, blocked/boundary cases fall back safely\n");
}

static void test_aligned_alloc(void){
    /* alignment <= ECCLES_RT_ALIGNMENT and a power of two: succeeds,
       same as a plain malloc (every allocation is already this aligned) */
    uint8_t *p = eccles_rt_aligned_alloc(ECCLES_RT_ALIGNMENT, 16);
    assert(p != NULL);
    assert(((uintptr_t)p % ECCLES_RT_ALIGNMENT) == 0);
    assert(eccles_rt_free(p) == ECCLES_RT_FREE_OK);

    /* alignment bigger than the built-in guarantee: rejected, not silently
       under-aligned */
    assert(eccles_rt_aligned_alloc((size_t)ECCLES_RT_ALIGNMENT * 4, 16) == NULL);

    /* not a power of two: rejected */
    assert(eccles_rt_aligned_alloc(3, 16) == NULL);

    check_counts(0, 0, 0, "aligned_alloc test touched nothing on the rejected paths");
    printf("PASS aligned_alloc: succeeds within the built-in guarantee, rejects anything beyond it\n");
}

static void test_reset(void){
    uint8_t *a = eccles_rt_malloc(1);
    uint8_t *b = eccles_rt_malloc(A_SIZE + 1);
    uint8_t *c = eccles_rt_malloc(C_SIZE);
    assert(a && b && c);
    check_counts(1, 1, 1, "before reset");

    assert(eccles_rt_reset() == true);
    check_counts(0, 0, 0, "reset cleared every pool's bookkeeping");

    /* the allocator is immediately usable again, no re-init needed */
    uint8_t *fresh = eccles_rt_malloc(1);
    assert(fresh != NULL);
    assert(eccles_rt_free(fresh) == ECCLES_RT_FREE_OK);
    check_counts(0, 0, 0, "usable immediately after reset");

    printf("PASS eccles_rt_reset() clears bookkeeping and stays usable\n");
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
    test_small_allocation_escalation();
    test_full_exhaustion_returns_null();
    test_pool_exhaustion_and_reuse();
    test_fragmentation();
    test_repeated_init();
    test_calloc();
    test_realloc();
    test_realloc_in_place();
    test_aligned_alloc();
    test_reset();

    printf("\nALL UNIT TESTS PASSED\n");
    return 0;
}
