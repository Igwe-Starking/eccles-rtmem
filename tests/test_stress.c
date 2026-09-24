/*
    Randomized stress test with invariant checking, addressing the review
    items "no randomized stress testing" and "testing should include
    corruption-invariant checks".

    Strategy: keep a host-side table of every buffer we currently own.
    Each cycle, either allocate a random size and stamp it with a unique
    canary byte pattern, or free a random buffer we own after first
    verifying its canary is intact (proves nothing else wrote into it -
    i.e. no overlap between "concurrent" allocations). Every N cycles,
    verify eccles_rt_get_stats()'s used+free counts still add up.

    Run with:
        gcc -std=c99 -Wall -Wextra -I.. ../eccles_rtmem.c test_stress.c -o test_stress
        ./test_stress [operation_count] [seed]
*/
#include "eccles_rtmem.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LIVE 4096
#define DEFAULT_OPS 500000
#define CHECK_EVERY 500

typedef struct {
    uint8_t *ptr;
    size_t   size;
    uint8_t  canary;
} live_alloc_t;

static live_alloc_t live[MAX_LIVE];
static size_t liveCount = 0;

static void check_invariants(void){
    eccles_rt_stats_t s = eccles_rt_get_stats();
    assert((unsigned)s.usedA + s.freeA == ECCLES_RT_POOL_A_COUNT);
    assert((unsigned)s.usedB + s.freeB == ECCLES_RT_POOL_B_COUNT);
    assert((unsigned)s.usedC + s.freeC == ECCLES_RT_POOL_C_COUNT);
    /* every live buffer's canary must still be intact - a corrupted
       canary means some other allocation wrote outside its own bounds */
    size_t i, j;
    for(i = 0; i < liveCount; i++){
        for(j = 0; j < live[i].size; j++){
            if(live[i].ptr[j] != live[i].canary){
                printf("CORRUPTION: buffer %zu (ptr=%p size=%zu canary=0x%02x) byte %zu is 0x%02x\n",
                    i, (void*)live[i].ptr, live[i].size, live[i].canary, j, live[i].ptr[j]);
                assert(0);
            }
        }
    }
}

int main(int argc, char **argv){
    long ops = (argc > 1) ? atol(argv[1]) : DEFAULT_OPS;
    unsigned seed = (argc > 2) ? (unsigned)atol(argv[2]) : 12345u;
    long i;
    long allocAttempts = 0, allocSuccesses = 0, freeCount = 0;

    srand(seed);
    assert(eccles_rtmem_init() == true);

    printf("running %ld randomized operations (seed=%u)...\n", ops, seed);

    for(i = 0; i < ops; i++){
        /* bias slightly toward freeing when we're holding a lot of live
           buffers, so the table doesn't just grow until malloc starts
           failing every time and the test stops exercising free() */
        int doFree = (liveCount > 0) && ((liveCount >= MAX_LIVE) || (rand() % 100 < 45));

        if(doFree){
            size_t idx = (size_t)rand() % liveCount;
            /* canary already verified as part of check_invariants below;
               spot-check it here too right before freeing */
            size_t j;
            for(j = 0; j < live[idx].size; j++){
                assert(live[idx].ptr[j] == live[idx].canary);
            }
            eccles_rt_free_status_t st = eccles_rt_free(live[idx].ptr);
            assert(st == ECCLES_RT_FREE_OK);
            freeCount++;
            live[idx] = live[liveCount - 1];
            liveCount--;
        } else {
            /* mix of small, mid, and occasionally large/multi-block sizes */
            size_t size;
            int r = rand() % 100;
            if(r < 60)       size = 1 + (size_t)(rand() % ECCLES_RT_BLOCK_A_SIZE);
            else if(r < 85)  size = ECCLES_RT_BLOCK_A_SIZE + 1 + (size_t)(rand() % ECCLES_RT_BLOCK_B_SIZE);
            else if(r < 97)  size = ECCLES_RT_BLOCK_B_SIZE + 1 + (size_t)(rand() % ECCLES_RT_BLOCK_C_SIZE);
            else             size = (size_t)ECCLES_RT_BLOCK_C_SIZE * (1 + (size_t)(rand() % 3)) + 1;

            allocAttempts++;
            uint8_t *p = eccles_rt_malloc(size);
            if(p != NULL){
                assert(liveCount < MAX_LIVE);
                uint8_t canary = (uint8_t)(rand() & 0xFF);
                memset(p, canary, size);
                live[liveCount].ptr = p;
                live[liveCount].size = size;
                live[liveCount].canary = canary;
                liveCount++;
                allocSuccesses++;
            }
            /* a NULL result here is expected under memory pressure - not
               a failure by itself, just logged in the summary below */
        }

        if(i % CHECK_EVERY == 0){
            check_invariants();
        }
    }

    /* free everything still live, verifying canaries one last time */
    check_invariants();
    while(liveCount > 0){
        size_t j;
        for(j = 0; j < live[liveCount - 1].size; j++){
            assert(live[liveCount - 1].ptr[j] == live[liveCount - 1].canary);
        }
        assert(eccles_rt_free(live[liveCount - 1].ptr) == ECCLES_RT_FREE_OK);
        liveCount--;
    }
    check_invariants();

    eccles_rt_stats_t finalStats = eccles_rt_get_stats();
    assert(finalStats.usedA == 0 && finalStats.usedB == 0 && finalStats.usedC == 0);

    printf("ops=%ld  alloc attempts=%ld  alloc successes=%ld (%.1f%%)  frees=%ld\n",
        ops, allocAttempts, allocSuccesses,
        allocAttempts ? (100.0 * (double)allocSuccesses / (double)allocAttempts) : 0.0,
        freeCount);
    printf("final state: all pools empty, no canary corruption detected\n");
    printf("\nSTRESS TEST PASSED\n");
    return 0;
}
