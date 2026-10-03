/*
    ECCLES RTLib - RuntimeMemory implementation
    Portable plain-C rewrite: same pool-allocator design as the original
    ESP-IDF version, generalized so pool sizes/counts fall out of
    ECCLES_RT_MEM_SIZE (and the block-size/percent knobs) instead of being
    hardcoded, and locking/logging are picked per-platform instead of
    assuming FreeRTOS is always available.
*/

#include "eccles_rtmem.h"
#include <string.h> /* memcpy/memset - used by eccles_rt_calloc/_realloc */

/* ===================================================================== *
 *  logging
 * ===================================================================== */

#if defined(ECCLES_RT_DEBUG)
  #ifndef ECCLES_RT_LOG_LINE
    #include <stdio.h>
    #define ECCLES_RT_LOG_LINE(msg) printf("%s\n", (msg))
  #endif
#else
  #ifndef ECCLES_RT_LOG_LINE
    #define ECCLES_RT_LOG_LINE(msg) ((void)0)
  #endif
#endif

/* ===================================================================== *
 *  locking
 *
 *  ECCLES_RT_MEM_NO_LOCK strips locking out entirely - wins over
 *  everything below it. Otherwise, ECCLES_RT_LOCK_INIT/_LOCK/_UNLOCK/
 *  _MUTEX_T can be predefined together by the user to plug in any RTOS's
 *  mutex; otherwise we auto-detect the best available default for the
 *  platform, checking RTOSes before bare-metal chip-specific fallbacks
 *  (an RTOS can in principle run on almost any of these chips).
 * ===================================================================== */

#if defined(ECCLES_RT_MEM_NO_LOCK)
  /* no locking at all: no mutex object, no interrupt masking. Only safe
     if eccles_rt_malloc/_free/_get_stats are always called from a single
     thread/task/ISR context, or you're doing your own coarser locking
     around whole call sequences yourself. */
  #define ECCLES_RT_MUTEX_T           uint8_t
  #define ECCLES_RT_LOCK_STATE_T      uint8_t
  #define ECCLES_RT_LOCK_INIT()       1
  #define ECCLES_RT_LOCK(m, st)       ((void)(m), (void)(st))
  #define ECCLES_RT_UNLOCK(m, st)     ((void)(m), (void)(st))
  #define ECCLES_RT_MUTEX_VALID(m)    1

#elif defined(ECCLES_RT_LOCK_INIT) && defined(ECCLES_RT_LOCK) && defined(ECCLES_RT_UNLOCK) && defined(ECCLES_RT_MUTEX_T) && defined(ECCLES_RT_LOCK_STATE_T)
  /* user supplied every piece already - nothing to do here */

#elif defined(__ZEPHYR__)
  #if defined(__has_include)
    #if __has_include(<zephyr/kernel.h>)
      #include <zephyr/kernel.h>     /* Zephyr >= 3.x */
    #else
      #include <kernel.h>            /* older Zephyr */
    #endif
  #else
    #include <zephyr/kernel.h>
  #endif
  static struct k_mutex eccles_rt__zephyr_mutex;
  #define ECCLES_RT_MUTEX_T           struct k_mutex*
  #define ECCLES_RT_LOCK_STATE_T      uint8_t /* unused: k_mutex needs no saved state */
  #define ECCLES_RT_LOCK_INIT()       (k_mutex_init(&eccles_rt__zephyr_mutex), &eccles_rt__zephyr_mutex)
  #define ECCLES_RT_LOCK(m, st)       (k_mutex_lock((m), K_FOREVER), (void)(st))
  #define ECCLES_RT_UNLOCK(m, st)     (k_mutex_unlock((m)), (void)(st))
  #define ECCLES_RT_MUTEX_VALID(m)    ((m) != NULL)

#elif defined(ESP_PLATFORM) || defined(INC_FREERTOS_H) || defined(ECCLES_RT_USE_FREERTOS) || defined(ARDUINO_ARCH_ESP32)
  #include "freertos/FreeRTOS.h"
  #include "freertos/semphr.h"
  #define ECCLES_RT_MUTEX_T           SemaphoreHandle_t
  #define ECCLES_RT_LOCK_STATE_T      uint8_t /* unused: a real semaphore needs no saved state */
  #define ECCLES_RT_LOCK_INIT()       xSemaphoreCreateMutex()
  #define ECCLES_RT_LOCK(m, st)       (xSemaphoreTake((m), portMAX_DELAY), (void)(st))
  #define ECCLES_RT_UNLOCK(m, st)     (xSemaphoreGive((m)), (void)(st))
  #define ECCLES_RT_MUTEX_VALID(m)    ((m) != NULL)

#elif defined(__has_include) && __has_include("cmsis_os2.h") && !defined(ECCLES_RT_NO_CMSIS_RTOS2)
  /* typical of STM32CubeMX-generated projects (CMSIS-RTOS2 wrapping
     either RTX5 or FreeRTOS underneath - either way this is the portable
     API CubeMX code calls, so we match it) */
  #include "cmsis_os2.h"
  #define ECCLES_RT_MUTEX_T           osMutexId_t
  #define ECCLES_RT_LOCK_STATE_T      uint8_t /* unused: osMutex needs no saved state */
  #define ECCLES_RT_LOCK_INIT()       osMutexNew(NULL)
  #define ECCLES_RT_LOCK(m, st)       (osMutexAcquire((m), osWaitForever), (void)(st))
  #define ECCLES_RT_UNLOCK(m, st)     (osMutexRelease((m)), (void)(st))
  #define ECCLES_RT_MUTEX_VALID(m)    ((m) != NULL)

#elif defined(PICO_BUILD) || defined(PICO_ON_DEVICE) || defined(ARDUINO_ARCH_RP2040) || defined(PICO_RP2040) || defined(PICO_RP2350)
  /* RP2040/RP2350 are dual-core: a plain interrupt-mask critical section
     on one core would NOT stop the other core from touching the pools at
     the same time, so this needs the SDK's real (spinlock-backed)
     critical_section_t rather than the generic Cortex-M fallback below.
     critical_section_t already keeps its own saved IRQ state inside the
     struct itself (not in a variable of ours), so `st` goes unused here -
     note this does mean re-entering the SAME critical_section_t before
     exiting it will deadlock the calling core, same as any non-recursive
     spinlock; that's a Pico SDK property, not something this header can
     change. */
  #include "pico/critical_section.h"
  static critical_section_t eccles_rt__pico_cs;
  #define ECCLES_RT_MUTEX_T           uint8_t
  #define ECCLES_RT_LOCK_STATE_T      uint8_t
  #define ECCLES_RT_LOCK_INIT()       (critical_section_init(&eccles_rt__pico_cs), 1)
  #define ECCLES_RT_LOCK(m, st)       (critical_section_enter_blocking(&eccles_rt__pico_cs), (void)(m), (void)(st))
  #define ECCLES_RT_UNLOCK(m, st)     (critical_section_exit(&eccles_rt__pico_cs), (void)(m), (void)(st))
  #define ECCLES_RT_MUTEX_VALID(m)    ((m) != 0)

#elif defined(__AVR__)
  /* plain Arduino Uno/Nano/Mega: no RTOS, no preemption - a short global
     interrupt-disable is the standard poor-man's critical section.
     `st` is a variable local to the calling function (see eccles_rtmem.c),
     not a shared static, so nested/re-entrant saves at different call
     sites can't stomp each other. */
  #include <avr/interrupt.h>
  #define ECCLES_RT_MUTEX_T           uint8_t
  #define ECCLES_RT_LOCK_STATE_T      uint8_t
  #define ECCLES_RT_LOCK_INIT()       1
  #define ECCLES_RT_LOCK(m, st)       do { (st) = SREG; cli(); (void)(m); } while (0)
  #define ECCLES_RT_UNLOCK(m, st)     do { SREG = (st); (void)(m); } while (0)
  #define ECCLES_RT_MUTEX_VALID(m)    ((m) != 0)

#elif defined(__MSP430__)
  /* save/restore the interrupt-enable bit rather than blindly
     re-enabling, so a lock taken while interrupts were already off
     doesn't accidentally turn them back on. `st` is call-site-local. */
  #include <msp430.h>
  #define ECCLES_RT_MUTEX_T           uint8_t
  #define ECCLES_RT_LOCK_STATE_T      __istate_t
  #define ECCLES_RT_LOCK_INIT()       1
  #define ECCLES_RT_LOCK(m, st)       do { (st) = __get_interrupt_state(); __disable_interrupt(); (void)(m); } while (0)
  #define ECCLES_RT_UNLOCK(m, st)     do { __set_interrupt_state(st); (void)(m); } while (0)
  #define ECCLES_RT_MUTEX_VALID(m)    ((m) != 0)

#elif defined(__XC16__) || defined(__XC32__)
  /* Microchip dsPIC/PIC24 (XC16) and PIC32 (XC32): __builtin_disable_
     interrupts() returns the previous ISR state, restore it exactly
     with __builtin_set_isr_state() rather than unconditionally
     re-enabling. `st` is call-site-local. */
  #define ECCLES_RT_MUTEX_T           uint8_t
  #define ECCLES_RT_LOCK_STATE_T      int
  #define ECCLES_RT_LOCK_INIT()       1
  #define ECCLES_RT_LOCK(m, st)       do { (st) = __builtin_disable_interrupts(); (void)(m); } while (0)
  #define ECCLES_RT_UNLOCK(m, st)     do { __builtin_set_isr_state(st); (void)(m); } while (0)
  #define ECCLES_RT_MUTEX_VALID(m)    ((m) != 0)

#elif defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) \
   || defined(__ARM_ARCH_8M_BASE__) || defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__) \
   || defined(__CORTEX_M)
  /* Cortex-M specifically (not "any ARM"). PRIMASK is an M-profile-only
     special register - it doesn't exist on Cortex-A/R, where the
     equivalent CPSR-based interrupt masking uses different instructions
     entirely, so this used to be gated on plain __arm__/__ARM_ARCH, which
     also matches Cortex-A/R (and even older ARM7TDMI-class cores),
     meaning an `mrs PRIMASK` on one of those would be an invalid
     instruction for that core. __ARM_ARCH_*M*__ are the macros GCC/Clang
     define from -mcpu=cortex-mN; __CORTEX_M is populated by CMSIS's
     core_cm*.h if that's been included instead. A plain ARM target that
     matches neither now correctly falls through to the no-op fallback
     below rather than risking a bad instruction. Covers STM32 (any line
     not already matched), SAMD, nRF52 without Zephyr/mbed's own
     scheduler, RP2040 if the Pico SDK headers above weren't reachable,
     etc. - save/disable/restore PRIMASK into a call-site-local `st`,
     syntax depends on toolchain */
  #define ECCLES_RT_MUTEX_T           uint8_t
  #define ECCLES_RT_LOCK_INIT()       1
  #define ECCLES_RT_MUTEX_VALID(m)    ((m) != 0)
  #if defined(__ICCARM__)
    /* IAR */
    #include <intrinsics.h>
    #define ECCLES_RT_LOCK_STATE_T    __istate_t
    #define ECCLES_RT_LOCK(m, st)     do { (st) = __get_interrupt_state(); __disable_interrupt(); (void)(m); } while (0)
    #define ECCLES_RT_UNLOCK(m, st)   do { __set_interrupt_state(st); (void)(m); } while (0)
  #elif defined(__ARMCC_VERSION)
    /* Arm Compiler (Keil, armclang/armcc) - __disable_irq() returns the
       previous PRIMASK (0 = was enabled, nonzero = was already disabled) */
    #define ECCLES_RT_LOCK_STATE_T    uint32_t
    #define ECCLES_RT_LOCK(m, st)     do { (st) = __disable_irq(); (void)(m); } while (0)
    #define ECCLES_RT_UNLOCK(m, st)   do { if ((st) == 0) __enable_irq(); (void)(m); } while (0)
  #else
    /* GCC / Clang: no CMSIS dependency needed, just PRIMASK directly */
    #define ECCLES_RT_LOCK_STATE_T    uint32_t
    #define ECCLES_RT_LOCK(m, st)     do { __asm volatile ("mrs %0, PRIMASK\n\tcpsid i" : "=r" (st) :: "memory"); (void)(m); } while (0)
    #define ECCLES_RT_UNLOCK(m, st)   do { __asm volatile ("msr PRIMASK, %0" :: "r" (st) : "memory"); (void)(m); } while (0)
  #endif

#else
  /* generic fallback: assumes a single-threaded, non-preemptive bare-metal
     build. Covers 8-bit chips without a case above (STM8, 8-bit PIC,
     8051), since their interrupt-disable syntax varies by toolchain
     (Cosmic/IAR/SDCC for STM8; EA=0/EA=1 for 8051, spelled differently
     across Keil C51/SDCC) too much to guess portably here - and covers
     anything else this header doesn't recognize. If you're on one of
     these and need real locking, define ECCLES_RT_LOCK_INIT/_LOCK/
     _UNLOCK/_MUTEX_T/_LOCK_STATE_T yourself, e.g. for SDCC-on-8051:
     LOCK -> save EA, then EA = 0; UNLOCK -> restore EA from the saved
     call-site-local value (not a shared static, so nested saves at
     different call sites don't stomp each other). */
  #define ECCLES_RT_MUTEX_T           uint8_t
  #define ECCLES_RT_LOCK_STATE_T      uint8_t
  #define ECCLES_RT_LOCK_INIT()       1
  #define ECCLES_RT_LOCK(m, st)       ((void)(m), (void)(st))
  #define ECCLES_RT_UNLOCK(m, st)     ((void)(m), (void)(st))
  #define ECCLES_RT_MUTEX_VALID(m)    ((m) != 0)
#endif

#ifndef ECCLES_RT_MUTEX_VALID
  #define ECCLES_RT_MUTEX_VALID(m) ((m) != NULL)
#endif

/* ===================================================================== *
 *  pool storage: static (.bss) by default, or heap-backed if
 *  ECCLES_RT_MEM_USE_HEAP is defined
 * ===================================================================== */

/* reuse the header's formula (already validated against ECCLES_RT_MEM_SIZE
   there) rather than duplicating the 3-term sum here; the (size_t) cast is
   still applied here for runtime array-sizing/pointer-arithmetic safety,
   which the header's #if-only version can't use (the preprocessor doesn't
   understand C casts) */
#define ECCLES_RT__POOL_BYTES ((size_t)ECCLES_RT__ACTUAL_POOL_BYTES)

#define ECCLES_RT__OFFSET_A ((size_t)0)
#define ECCLES_RT__OFFSET_B (ECCLES_RT__OFFSET_A + (size_t)ECCLES_RT_POOL_A_COUNT * ECCLES_RT_BLOCK_A_SIZE)
#define ECCLES_RT__OFFSET_C (ECCLES_RT__OFFSET_B + (size_t)ECCLES_RT_POOL_B_COUNT * ECCLES_RT_BLOCK_B_SIZE)

#define ECCLES_RT__REGBASE_A (0)
#define ECCLES_RT__REGBASE_B (ECCLES_RT_POOL_A_COUNT)
#define ECCLES_RT__REGBASE_C (ECCLES_RT_POOL_A_COUNT + ECCLES_RT_POOL_B_COUNT)

/* every pointer eccles_rt_malloc() hands out is aligned to
   ECCLES_RT_ALIGNMENT bytes. In heap mode this is done manually (over-
   allocate by up to ECCLES_RT_ALIGNMENT-1 extra bytes, then round the
   pointer up) rather than just trusting malloc()'s own alignment
   guarantee, which the C standard only promises up to alignof(max_align_t)
   - commonly 8 or 16 bytes - and ECCLES_RT_ALIGNMENT can be configured
   higher than that. In static mode it's applied via a GCC/Clang/ARM
   Compiler alignment attribute; on another toolchain in static mode,
   align eccles_rt_pool yourself with your compiler's own attribute/pragma
   if you need to cast returned pointers to wider types. */
#if defined(ECCLES_RT_MEM_USE_HEAP)
  #include <stdlib.h>
  static uint8_t *eccles_rt_pool_raw = NULL; /* the actual malloc()'d pointer */
  static uint8_t *eccles_rt_pool = NULL;     /* eccles_rt_pool_raw, rounded up to ECCLES_RT_ALIGNMENT */
  static uint8_t *eccles_rt_reg  = NULL;
#elif defined(__GNUC__) || defined(__clang__) || defined(__ARMCC_VERSION)
  static uint8_t eccles_rt_pool[ECCLES_RT__POOL_BYTES] __attribute__((aligned(ECCLES_RT_ALIGNMENT)));
  static uint8_t eccles_rt_reg[ECCLES_RT_TOTAL_BLOCKS];
#else
  static uint8_t eccles_rt_pool[ECCLES_RT__POOL_BYTES];
  static uint8_t eccles_rt_reg[ECCLES_RT_TOTAL_BLOCKS];
#endif

/* CONT_MARK: written into every block after the first in a multi-block
   run, distinct from any real run-length value (a run this long is
   already impossible - no pool holds 255 blocks) so freeBuffer can tell
   "this is a run start" apart from "this is mid-run" */
#define ECCLES_RT_CONT_MARK 255u

/* ===================================================================== *
 *  occupancy bitmap (1 = block in use, 0 = block free)
 *
 *  A packed second view of eccles_rt_reg[]: bit i is 1 exactly when
 *  eccles_rt_reg[i] != 0. eccles_rt_reg[] stays the source of truth for
 *  run lengths (freeBuffer/resolve/realloc read it); the bitmap exists so
 *  the search can jump to the next free block / end of a free run with one
 *  count-trailing-zeros per 64 blocks instead of testing bytes one by one.
 *  ECCLES_RT_TOTAL_BLOCKS <= 255 (header-enforced), so at most 4 words.
 * ===================================================================== */

#define ECCLES_RT__BM_WORDS ((ECCLES_RT_TOTAL_BLOCKS + 63) / 64)

/* XOR masks for eccles_rt__bm_next: look for a FREE (0) or USED (1) bit */
#define ECCLES_RT__FIND_FREE (~(uint64_t)0)
#define ECCLES_RT__FIND_USED ((uint64_t)0)

static uint64_t eccles_rt_bits[ECCLES_RT__BM_WORDS];

/* ctzll is undefined for 0 - every caller below tests the word != 0 first */
#if defined(__GNUC__) || defined(__clang__) || defined(__ARMCC_VERSION)
  #define ECCLES_RT__CTZ64(x) ((uint8_t)__builtin_ctzll((unsigned long long)(x)))
#else
  static uint8_t eccles_rt__ctz64(uint64_t x){
      uint8_t n = 0;
      while((x & 1u) == 0u){ x >>= 1; n++; }
      return n;
  }
  #define ECCLES_RT__CTZ64(x) eccles_rt__ctz64(x)
#endif

/* first index in [from, limit) whose bit matches `find` (one of the
   ECCLES_RT__FIND_* masks), or `limit` if there is none. Always called
   with a constant mask, so after inlining the XOR folds away. */
static uint16_t eccles_rt__bm_next(uint16_t from, uint16_t limit, uint64_t find){
    while(from < limit){
        /* shift so bit 0 is `from`; vacated high bits read as 0 = "no match" */
        uint64_t w = (eccles_rt_bits[from >> 6] ^ find) >> (from & 63u);
        if(w != 0){
            uint16_t idx = (uint16_t)(from + ECCLES_RT__CTZ64(w));
            return (idx < limit) ? idx : limit;
        }
        from = (uint16_t)((from | 63u) + 1u); /* start of the next word */
    }
    return limit;
}

/* set (value != 0) or clear n bits starting at `start`, a whole word's
   worth of bits per iteration (n <= 255, so at most 5 iterations) */
static void eccles_rt__bm_set(uint8_t start, uint8_t n, uint8_t value){
    uint16_t k = start, end = (uint16_t)start + n;
    while(k < end){
        uint8_t  sh   = (uint8_t)(k & 63u);
        uint16_t span = (uint16_t)(64u - sh);
        uint64_t m;
        if(span > (uint16_t)(end - k)) span = (uint16_t)(end - k);
        m = (span == 64u) ? ~(uint64_t)0 : ((((uint64_t)1 << span) - 1u) << sh);
        if(value) eccles_rt_bits[k >> 6] |= m;
        else      eccles_rt_bits[k >> 6] &= ~m;
        k = (uint16_t)(k + span);
    }
}

static void eccles_rt__bm_clear_all(void){
    uint8_t w;
    for(w = 0; w < ECCLES_RT__BM_WORDS; w++) eccles_rt_bits[w] = 0;
}

/* first run start s in [lo, hi] (inclusive) with `size` consecutive free
   blocks starting at s, or 0xFFFF. Only the START is bounded by hi; the
   caller guarantees hi + size <= pool end. Each step costs two ctzll's
   regardless of how long the used/free stretches are. */
static uint16_t eccles_rt__find_run(uint16_t lo, uint16_t hi, uint8_t size){
    uint16_t pos = lo;
    while(pos <= hi){
        uint16_t f = eccles_rt__bm_next(pos, (uint16_t)(hi + 1u), ECCLES_RT__FIND_FREE);
        uint16_t u;
        if(f > hi) break;
        u = eccles_rt__bm_next(f, (uint16_t)(f + size), ECCLES_RT__FIND_USED); /* capped at f+size */
        if(u == (uint16_t)(f + size)) return f;
        pos = (uint16_t)(u + 1u); /* resume just past the blocker */
    }
    return 0xFFFFu;
}

/* ===================================================================== *
 *  pool descriptors - table-driven instead of duplicating the same
 *  search/free logic three times (smaller code on flash-constrained parts)
 * ===================================================================== */

typedef struct {
    size_t   offset;     /* byte offset of this pool inside eccles_rt_pool */
    size_t   blockSize;  /* bytes per block */
    uint8_t  shift;      /* log2(blockSize), or 0xFF if blockSize isn't a power of two */
    uint8_t  count;      /* number of blocks in this pool */
    uint8_t  regBase;    /* index into eccles_rt_reg[] where this pool starts */
    uint8_t  cursor;     /* absolute registry index: where the next search starts */
    uint8_t  usedBlocks;  /* running count of blocks currently in use, kept in
                             sync by getFree/freeBuffer so eccles_rt_get_stats()
                             doesn't need to rescan the whole registry under
                             the lock every time it's called */
} eccles_rt_pool_t;

static eccles_rt_pool_t eccles_rt_pools[3];
static ECCLES_RT_MUTEX_T eccles_rt_lock;

/* pool indices, in the fallback order eccles_rt_malloc uses for oversized requests */
#define POOL_A 0
#define POOL_B 1
#define POOL_C 2

#if defined(ECCLES_RT_MALLOC_MIN_WASTE)
/* named struct (not an anonymous/typeof-based one) so the sort below is
   plain, portable C99 - no compiler extensions needed on toolchains that
   don't support typeof (IAR, Keil ARMCC5, XC8/16/32, ...) */
typedef struct {
    uint8_t poolIdx;
    size_t  need;
    size_t  waste;
    bool    feasible;
} eccles_rt__waste_candidate_t;
#endif

static uint8_t eccles_rt__log2_exact(size_t v){
    uint8_t shift = 0;
    if(v == 0) return 0xFF;
    while((v & 1u) == 0u){ v >>= 1; shift++; }
    return (v == 1u) ? shift : 0xFF; /* 0xFF => not a power of two */
}

/* blocks of pool *p needed to hold `size` bytes (ceil division). Uses the
   pool's shift/mask when its block size is a power of two, so the common
   case never touches a divide (a library call on AVR/MSP430/Cortex-M0),
   and never forms size + blockSize - 1, so it can't overflow for huge sizes. */
static size_t eccles_rt__blocks_for(const eccles_rt_pool_t *p, size_t size){
    if(p->shift != 0xFF){
        return (size >> p->shift) + (((size & (p->blockSize - 1)) != 0) ? 1u : 0u);
    }
    return size / p->blockSize + (((size % p->blockSize) != 0) ? 1u : 0u);
}

/* ===================================================================== *
 *  allocation core
 * ===================================================================== */

static uint8_t* eccles_rt_getFree(uint8_t size, uint8_t poolIdx){
    eccles_rt_pool_t *p = &eccles_rt_pools[poolIdx];
    uint8_t  rsb = p->regBase;
    uint16_t end = (uint16_t)rsb + p->count; /* one past this pool's last registry index */
    uint16_t cur = p->cursor;
    uint16_t i;
    size_t   byteOffset;

    /* O(1) reject: fewer free blocks than requested means no run can
       exist (this also covers size > count). It is what makes the
       escalation chain in eccles_rt_malloc cheap - a full pool costs one
       compare, not a scan. */
    if(size == 0 || (uint8_t)(p->count - p->usedBlocks) < size){
        return NULL;
    }

    if(size == 1){
        /* single block: the check above proved at least one free bit
           exists in this pool, so if [cursor, end) has none, [rsb, cursor)
           is guaranteed to - no failure path, one ctzll in the common case */
        i = eccles_rt__bm_next(cur, end, ECCLES_RT__FIND_FREE);
        if(i == end) i = eccles_rt__bm_next(rsb, cur, ECCLES_RT__FIND_FREE);
    } else {
        /* circular first-fit: candidate starts are tried cursor..end-size,
           then rsb..cursor-1, and a run may never start so late that it
           would cross this pool's own boundary. All index math is uint16_t
           so a pool ending near index 256 can't wrap a uint8_t. */
        uint16_t lastStart = (uint16_t)(end - size);
        i = (cur <= lastStart) ? eccles_rt__find_run(cur, lastStart, size) : 0xFFFFu;
        if(i == 0xFFFFu && cur > rsb){
            uint16_t hi = ((uint16_t)(cur - 1u) < lastStart) ? (uint16_t)(cur - 1u) : lastStart;
            i = eccles_rt__find_run(rsb, hi, size);
        }
        if(i == 0xFFFFu) return NULL;
    }

    /* commit: registry run length + continuation marks, bitmap, counter */
    eccles_rt_reg[i] = size;
    if(size == 1){
        eccles_rt_bits[i >> 6] |= (uint64_t)1 << (i & 63u);
    } else {
        memset(&eccles_rt_reg[i + 1u], (int)ECCLES_RT_CONT_MARK, (size_t)size - 1u);
        eccles_rt__bm_set((uint8_t)i, size, 1);
    }
    p->usedBlocks = (uint8_t)(p->usedBlocks + size);

    {
        uint16_t next = (uint16_t)(i + size);
        p->cursor = (uint8_t)((next >= end) ? rsb : next);
    }

    if(p->shift != 0xFF){
        byteOffset = ((size_t)(i - rsb) << p->shift) + p->offset;
    } else {
        byteOffset = ((size_t)(i - rsb) * p->blockSize) + p->offset;
    }

    /* deliberately no logging here - this can run inside the lock (called
       from eccles_rt_malloc, possibly more than once per call in the
       oversized-request fallback chain), and ECCLES_RT_LOG_LINE can be a
       slow I/O call (Serial.println, printf, ...) that has no business
       running while interrupts are masked or another task is blocked on
       this mutex. eccles_rt_malloc logs once, after unlocking, if every
       attempt failed. */
    return &eccles_rt_pool[byteOffset];
}

/* Shared by eccles_rt_freeBuffer() and eccles_rt_realloc(): resolves an
   allocated pointer back to its registry slot. Returns ECCLES_RT_FREE_OK
   (despite the name, this isn't freeing anything - it's just validation)
   with *outPool, *outRind, and *outRunBlocks filled in, or the specific
   rejection status otherwise. Kept separate from eccles_rt_freeBuffer so
   realloc doesn't need to duplicate this validation, or free-then-
   reallocate-the-registry-slot just to inspect it. */
static eccles_rt_status_t eccles_rt__resolve(uint8_t *buffer, eccles_rt_pool_t **outPool, uint8_t *outRind, uint8_t *outRunBlocks){
    uintptr_t base = (uintptr_t)eccles_rt_pool;
    uintptr_t addr = (uintptr_t)buffer;
    size_t oft;
    eccles_rt_pool_t *p;
    uint8_t rind, mb;

    if(addr < base || (addr - base) >= ECCLES_RT__POOL_BYTES){
        return ECCLES_RT_FREE_OUT_OF_RANGE;
    }
    oft = (size_t)(addr - base);

    if(oft < eccles_rt_pools[POOL_B].offset){
        p = &eccles_rt_pools[POOL_A];
    } else if(oft < eccles_rt_pools[POOL_C].offset){
        p = &eccles_rt_pools[POOL_B];
    } else {
        p = &eccles_rt_pools[POOL_C];
    }

    if(p->shift != 0xFF){
        if(((oft - p->offset) & (p->blockSize - 1)) != 0) return ECCLES_RT_FREE_MISALIGNED;
        rind = (uint8_t)(((oft - p->offset) >> p->shift) + p->regBase);
    } else {
        if(((oft - p->offset) % p->blockSize) != 0) return ECCLES_RT_FREE_MISALIGNED;
        rind = (uint8_t)(((oft - p->offset) / p->blockSize) + p->regBase);
    }

    mb = eccles_rt_reg[rind];
    if(mb == 0) return ECCLES_RT_FREE_ALREADY_FREE; /* i.e. "not currently allocated" */
    if(mb == (uint8_t)ECCLES_RT_CONT_MARK) return ECCLES_RT_FREE_MID_RUN;

    *outPool = p;
    *outRind = rind;
    *outRunBlocks = mb;
    return ECCLES_RT_FREE_OK;
}

static eccles_rt_status_t eccles_rt_freeBuffer(uint8_t* buffer){
    eccles_rt_pool_t *p;
    uint8_t rind, mb;
    eccles_rt_status_t st = eccles_rt__resolve(buffer, &p, &rind, &mb);

    /* no logging here (see the "deliberately no logging" note in getFree
       above - same reasoning, this runs inside the lock); eccles_rt_free
       logs once, after unlocking, based on the status this returns. */
    if(st != ECCLES_RT_FREE_OK) return st;

    memset(&eccles_rt_reg[rind], 0, mb);
    eccles_rt__bm_set(rind, mb, 0);
    p->usedBlocks = (uint8_t)(p->usedBlocks - mb);

    /* zeroing every freed block is skipped on purpose - costs up to
       blockSize * mb bytes for nothing most callers need. If you need a
       zeroed buffer, memset it yourself right after eccles_rt_malloc(). */
    return ECCLES_RT_FREE_OK;
}

/* ===================================================================== *
 *  public API
 * ===================================================================== */

/* readiness gate, decoupled from mutex validity: ECCLES_RT_MEM_NO_LOCK
   mode has no real mutex object to check, and even outside that mode a
   valid-looking mutex handle doesn't by itself mean init actually ran */
static bool eccles_rt_initialized = false;

bool eccles_rtmem_init(void){
    ECCLES_RT_LOCK_STATE_T lockState;

    /* idempotent: a second call is a safe no-op rather than re-malloc()ing
       a fresh pool (heap mode) or resetting cursors (static mode) out from
       under buffers already handed out by an earlier eccles_rt_malloc() */
    if(eccles_rt_initialized) return true;

#if defined(ECCLES_RT_MEM_USE_HEAP)
    /* over-allocate by up to ECCLES_RT_ALIGNMENT-1 extra bytes and round
       the pointer up ourselves, rather than trusting malloc() to already
       satisfy ECCLES_RT_ALIGNMENT - the C standard only guarantees
       malloc() aligns to alignof(max_align_t) (commonly 8 or 16 bytes),
       and ECCLES_RT_ALIGNMENT can be configured higher than that. This
       works portably on any C99 implementation, no compiler-specific
       attribute needed. ECCLES_RT_ALIGNMENT is validated to be a power of
       two in the header, which is what makes the bitmask rounding below
       valid. */
    eccles_rt_pool_raw = (uint8_t*)malloc(ECCLES_RT__POOL_BYTES + ECCLES_RT_ALIGNMENT - 1);
    eccles_rt_reg      = (uint8_t*)malloc(ECCLES_RT_TOTAL_BLOCKS);
    if(eccles_rt_pool_raw == NULL || eccles_rt_reg == NULL){
        ECCLES_RT_LOG_LINE("eccles_rtmem: heap allocation failed in eccles_rtmem_init");
        free(eccles_rt_pool_raw); eccles_rt_pool_raw = NULL;
        free(eccles_rt_reg);      eccles_rt_reg      = NULL;
        return false; /* stays uninitialized - call again later to retry */
    }
    eccles_rt_pool = (uint8_t*)(((uintptr_t)eccles_rt_pool_raw + (ECCLES_RT_ALIGNMENT - 1))
                                 & ~(uintptr_t)(ECCLES_RT_ALIGNMENT - 1));
    {
        uint16_t k;
        for(k = 0; k < ECCLES_RT_TOTAL_BLOCKS; k++) eccles_rt_reg[k] = 0;
    }
#endif

    eccles_rt__bm_clear_all();

    eccles_rt_pools[POOL_A].offset    = ECCLES_RT__OFFSET_A;
    eccles_rt_pools[POOL_A].blockSize = ECCLES_RT_BLOCK_A_SIZE;
    eccles_rt_pools[POOL_A].count     = ECCLES_RT_POOL_A_COUNT;
    eccles_rt_pools[POOL_A].regBase   = ECCLES_RT__REGBASE_A;
    eccles_rt_pools[POOL_A].cursor    = ECCLES_RT__REGBASE_A;
    eccles_rt_pools[POOL_A].shift     = eccles_rt__log2_exact(ECCLES_RT_BLOCK_A_SIZE);
    eccles_rt_pools[POOL_A].usedBlocks = 0;

    eccles_rt_pools[POOL_B].offset    = ECCLES_RT__OFFSET_B;
    eccles_rt_pools[POOL_B].blockSize = ECCLES_RT_BLOCK_B_SIZE;
    eccles_rt_pools[POOL_B].count     = ECCLES_RT_POOL_B_COUNT;
    eccles_rt_pools[POOL_B].regBase   = ECCLES_RT__REGBASE_B;
    eccles_rt_pools[POOL_B].cursor    = ECCLES_RT__REGBASE_B;
    eccles_rt_pools[POOL_B].shift     = eccles_rt__log2_exact(ECCLES_RT_BLOCK_B_SIZE);
    eccles_rt_pools[POOL_B].usedBlocks = 0;

    eccles_rt_pools[POOL_C].offset    = ECCLES_RT__OFFSET_C;
    eccles_rt_pools[POOL_C].blockSize = ECCLES_RT_BLOCK_C_SIZE;
    eccles_rt_pools[POOL_C].count     = ECCLES_RT_POOL_C_COUNT;
    eccles_rt_pools[POOL_C].regBase   = ECCLES_RT__REGBASE_C;
    eccles_rt_pools[POOL_C].cursor    = ECCLES_RT__REGBASE_C;
    eccles_rt_pools[POOL_C].shift     = eccles_rt__log2_exact(ECCLES_RT_BLOCK_C_SIZE);
    eccles_rt_pools[POOL_C].usedBlocks = 0;

    if(eccles_rt_pools[POOL_A].shift == 0xFF || eccles_rt_pools[POOL_B].shift == 0xFF || eccles_rt_pools[POOL_C].shift == 0xFF){
        ECCLES_RT_LOG_LINE("eccles_rtmem: a block size is not a power of two, falling back to slower division math for that pool");
    }

    eccles_rt_lock = (ECCLES_RT_MUTEX_T)ECCLES_RT_LOCK_INIT();
    (void)lockState; /* only declared for type-checking ECCLES_RT_LOCK_STATE_T exists; unused here */

    if(!ECCLES_RT_MUTEX_VALID(eccles_rt_lock)){
        /* an RTOS mutex/semaphore create call can fail (e.g. FreeRTOS's
           heap is exhausted) - previously this went unnoticed and
           eccles_rtmem_init() still reported success, after which every
           eccles_rt_malloc/_free call would silently no-op via their own
           ECCLES_RT_MUTEX_VALID guard, with nothing telling you why
           nothing was ever being allocated. Report it here instead. */
        ECCLES_RT_LOG_LINE("eccles_rtmem: lock/mutex creation failed in eccles_rtmem_init");
#if defined(ECCLES_RT_MEM_USE_HEAP)
        /* free the actual malloc()'d pointer, not the rounded-up one -
           freeing anything else would be undefined behavior */
        free(eccles_rt_pool_raw); eccles_rt_pool_raw = NULL; eccles_rt_pool = NULL;
        free(eccles_rt_reg);      eccles_rt_reg      = NULL;
#endif
        return false; /* stays uninitialized - call again later to retry */
    }

    eccles_rt_initialized = true;
    return true;
}

uint8_t* eccles_rt_malloc(size_t size){
    uint8_t *result = NULL;
    ECCLES_RT_LOCK_STATE_T lockState;

    if(size == 0) return NULL; /* matches malloc(0)'s common "return NULL" convention */
    if(!eccles_rt_initialized || !ECCLES_RT_MUTEX_VALID(eccles_rt_lock)) return NULL;
#if defined(ECCLES_RT_MEM_USE_HEAP)
    if(eccles_rt_pool == NULL || eccles_rt_reg == NULL) return NULL;
#endif

    /* held for the whole call, including the fallback chain below: getFree
       doesn't lock itself, so releasing between attempts would let another
       task's eccles_rt_malloc/_free interleave and corrupt the shared
       registry. lockState is local to this call, not shared, so this is
       safe even if another call to this same function is somehow already
       holding the lock on a different call stack (interrupt-mask backends;
       see the reentrancy notes in eccles_rtmem.h). */
    ECCLES_RT_LOCK(eccles_rt_lock, lockState);

    if(size <= ECCLES_RT_BLOCK_C_SIZE){
        /* fits in a single block of some class. Start at the smallest
           class that actually fits the request, then escalate to
           progressively larger classes if that one has no free block
           right now - never step down to a smaller class, since its
           blocks might be too small to hold the request at all. This
           means a small request doesn't fail just because pool A happens
           to be full while B or C has room - it costs nothing extra when
           the first attempt succeeds (the common case), since the loop
           exits immediately on the first hit. */
        static const uint8_t singleClassOrder[3] = { POOL_A, POOL_B, POOL_C };
        uint8_t startIdx = (size <= ECCLES_RT_BLOCK_A_SIZE) ? 0 :
                            (size <= ECCLES_RT_BLOCK_B_SIZE) ? 1 : 2;
        uint8_t oi;
        for(oi = startIdx; oi < 3 && result == NULL; oi++){
            result = eccles_rt_getFree(1, singleClassOrder[oi]);
        }
    } else {
        /* bigger than one C block: needs a contiguous multi-block run.
           This never combines blocks from two different pools into one
           allocation - each attempt is single-class only. */
#if defined(ECCLES_RT_MALLOC_MIN_WASTE)
        /* opt-in: try the class that wastes the fewest bytes on this
           particular request first, falling back to the next-least-
           wasteful class that's actually feasible (need <= 255) if the
           first choice is too fragmented to satisfy it. This can pull
           more blocks out of the SMALL pools for a single large request
           than the default order would (e.g. many A-blocks instead of a
           few C-blocks), which trades lower byte-waste on this call for
           less spare capacity in the pool your frequent small
           allocations actually depend on - see the ECCLES_RT_MALLOC_MIN_
           WASTE note in eccles_rtmem.h before turning this on. */
        eccles_rt__waste_candidate_t cand[3];
        eccles_rt__waste_candidate_t t;
        uint8_t ci, cj;
        /* populate largest-class-first (C, B, A): the sort below is a
           stable one (only swaps on a STRICT improvement), so when two
           classes tie exactly on waste - e.g. a request that happens to
           be a common multiple of more than one block size - the larger
           of the tied classes wins and stays ahead of the tie-breaking
           check below. That matches the same "prefer to leave the small
           pools alone" bias the default (non-MIN_WASTE) ordering uses,
           applied only as a tiebreaker here rather than the primary rule */
        static const uint8_t candPool[3] = { POOL_C, POOL_B, POOL_A };
        for(ci = 0; ci < 3; ci++){
            uint8_t poolIdx = candPool[ci];
            size_t bs = eccles_rt_pools[poolIdx].blockSize;
            size_t need = eccles_rt__blocks_for(&eccles_rt_pools[poolIdx], size);
            cand[ci].poolIdx = poolIdx;
            cand[ci].need = need;
            cand[ci].feasible = (need <= 255);
            cand[ci].waste = cand[ci].feasible ? (need * bs - size) : (size_t)-1;
        }
        /* fully unrolled 3-element insertion sort by ascending waste -
           always exactly 3 candidates, so this is cheaper and more
           predictable on a small MCU than a general sort routine */
        if(cand[1].waste < cand[0].waste){ t = cand[0]; cand[0] = cand[1]; cand[1] = t; }
        if(cand[2].waste < cand[1].waste){ t = cand[1]; cand[1] = cand[2]; cand[2] = t; }
        if(cand[1].waste < cand[0].waste){ t = cand[0]; cand[0] = cand[1]; cand[1] = t; }
        for(cj = 0; cj < 3 && result == NULL; cj++){
            if(cand[cj].feasible){
                result = eccles_rt_getFree((uint8_t)cand[cj].need, cand[cj].poolIdx);
            }
        }
#else
        /* default: largest-class-first. Tends to preserve the small
           pools' block-count capacity for their intended job (frequent
           small allocations) instead of spending it on one large
           request, at the cost of possibly wasting more bytes on that
           one request than a smaller-class choice would have. */
        static const uint8_t order[3] = { POOL_C, POOL_B, POOL_A };
        uint8_t oi;
        for(oi = 0; oi < 3 && result == NULL; oi++){
            uint8_t poolIdx = order[oi];
            size_t need = eccles_rt__blocks_for(&eccles_rt_pools[poolIdx], size);
            if(need <= 255){
                result = eccles_rt_getFree((uint8_t)need, poolIdx);
            }
        }
#endif
    }

    ECCLES_RT_UNLOCK(eccles_rt_lock, lockState);

    /* logged AFTER unlocking - ECCLES_RT_LOG_LINE can be a slow I/O call
       (Serial.println, printf, ...), which has no business running while
       another task is blocked waiting on this same lock */
    if(result == NULL){
        ECCLES_RT_LOG_LINE("eccles_rtmem: allocation failed, no pool had a large enough contiguous run free");
    }
    return result;
}

eccles_rt_status_t eccles_rt_free(uint8_t* buffer){
    eccles_rt_status_t status;
    ECCLES_RT_LOCK_STATE_T lockState;

    if(buffer == NULL) return ECCLES_RT_FREE_NULL;
    if(!eccles_rt_initialized) return ECCLES_RT_FREE_NOT_READY;
#if defined(ECCLES_RT_MEM_USE_HEAP)
    if(eccles_rt_pool == NULL) return ECCLES_RT_FREE_NOT_READY;
#endif
    if(!ECCLES_RT_MUTEX_VALID(eccles_rt_lock)) return ECCLES_RT_FREE_NOT_READY;

    ECCLES_RT_LOCK(eccles_rt_lock, lockState);
    status = eccles_rt_freeBuffer(buffer);
    ECCLES_RT_UNLOCK(eccles_rt_lock, lockState);

    /* logged AFTER unlocking, same reasoning as eccles_rt_malloc above */
    switch(status){
        case ECCLES_RT_FREE_MISALIGNED:
            ECCLES_RT_LOG_LINE("eccles_rtmem: eccles_rt_free pointer is not a block start, ignoring");
            break;
        case ECCLES_RT_FREE_ALREADY_FREE:
            ECCLES_RT_LOG_LINE("eccles_rtmem: eccles_rt_free buffer already free, ignoring double free");
            break;
        case ECCLES_RT_FREE_MID_RUN:
            ECCLES_RT_LOG_LINE("eccles_rtmem: eccles_rt_free pointer is mid-run, not a block start, ignoring");
            break;
        default:
            break; /* OK / NULL / OUT_OF_RANGE / NOT_READY need no extra log line */
    }
    return status;
}

uint8_t* eccles_rt_calloc(size_t count, size_t size){
    size_t total;
    uint8_t *result;

    if(count == 0 || size == 0) return NULL;

    /* overflow-safe multiply: if the multiply wrapped around, dividing
       back out won't recover the original count */
    total = count * size;
    if(total / size != count) return NULL;

    result = eccles_rt_malloc(total);
    if(result != NULL){
        memset(result, 0, total);
    }
    return result;
}

uint8_t* eccles_rt_realloc(uint8_t *ptr, size_t new_size){
    eccles_rt_pool_t *p;
    uint8_t rind, mb;
    size_t oldCapacity = 0;
    bool inPlace = false;
    eccles_rt_status_t st;
    ECCLES_RT_LOCK_STATE_T lockState;
    uint8_t *newPtr;

    if(ptr == NULL) return eccles_rt_malloc(new_size);
    if(new_size == 0){ eccles_rt_free(ptr); return NULL; }

    if(!eccles_rt_initialized) return NULL;
#if defined(ECCLES_RT_MEM_USE_HEAP)
    if(eccles_rt_pool == NULL) return NULL;
#endif
    if(!ECCLES_RT_MUTEX_VALID(eccles_rt_lock)) return NULL;

    /* resolve + resize-in-place happen in ONE critical section, so no
       other task can claim the neighbouring blocks between "I checked
       they're free" and "I took them". Only the move fallback below
       (malloc + copy + free) is split across separate lock sections,
       same as before - as with every realloc(), this is not safe against
       another thread freeing/reallocating the SAME pointer concurrently. */
    ECCLES_RT_LOCK(eccles_rt_lock, lockState);
    st = eccles_rt__resolve(ptr, &p, &rind, &mb);
    if(st == ECCLES_RT_FREE_OK){
        size_t need = eccles_rt__blocks_for(p, new_size);
        oldCapacity = (size_t)mb * p->blockSize;

        if(need == (size_t)mb){
            inPlace = true; /* same block count: nothing to touch */

        } else if(need < (size_t)mb){
            /* SHRINK: hand the excess tail blocks straight back to the pool.
               The run keeps its start, so the pointer doesn't move. Bytes
               are not zeroed (same policy as free). */
            uint8_t keep   = (uint8_t)need;
            uint8_t excess = (uint8_t)(mb - keep);
            eccles_rt_reg[rind] = keep;
            memset(&eccles_rt_reg[rind + keep], 0, excess);
            eccles_rt__bm_set((uint8_t)(rind + keep), excess, 0);
            p->usedBlocks = (uint8_t)(p->usedBlocks - excess);
            inPlace = true;

        } else if(need <= p->count){
            /* GROW: look at the boundary right after this run. If the next
               (need - mb) blocks are free and still inside this pool, absorb
               them - no copy, no move. */
            uint16_t poolEnd = (uint16_t)p->regBase + p->count;
            uint16_t tail    = (uint16_t)rind + mb;
            uint16_t newEnd  = (uint16_t)rind + (uint16_t)need;
            if(newEnd <= poolEnd && eccles_rt__bm_next(tail, newEnd, ECCLES_RT__FIND_USED) == newEnd){
                uint8_t extra = (uint8_t)(need - mb);
                eccles_rt_reg[rind] = (uint8_t)need;
                memset(&eccles_rt_reg[tail], (int)ECCLES_RT_CONT_MARK, extra);
                eccles_rt__bm_set((uint8_t)tail, extra, 1);
                p->usedBlocks = (uint8_t)(p->usedBlocks + extra);
                /* keep the next-fit cursor from pointing into the run we
                   just grew */
                if(p->cursor >= tail && p->cursor < newEnd){
                    p->cursor = (uint8_t)((newEnd >= poolEnd) ? p->regBase : newEnd);
                }
                inPlace = true;
            }
        }
    }
    ECCLES_RT_UNLOCK(eccles_rt_lock, lockState);

    if(st != ECCLES_RT_FREE_OK){
        /* an invalid pointer passed to realloc gets the same treatment as
           an invalid free(): don't touch anything, report failure */
        ECCLES_RT_LOG_LINE("eccles_rtmem: eccles_rt_realloc given a pointer this allocator doesn't recognize, ignoring");
        return NULL;
    }
    if(inPlace) return ptr;

    /* couldn't extend in place (neighbour in use, pool boundary, or the
       request is bigger than this whole pool): move to wherever it fits,
       possibly a different class. Here new_size > oldCapacity, so copying
       oldCapacity bytes is the full preserved content. */
    newPtr = eccles_rt_malloc(new_size);
    if(newPtr == NULL){
        return NULL; /* ptr is untouched and still valid - do not free it here */
    }

    memcpy(newPtr, ptr, oldCapacity);
    eccles_rt_free(ptr);
    return newPtr;
}

uint8_t* eccles_rt_aligned_alloc(size_t alignment, size_t size){
    if(alignment == 0 || (alignment & (alignment - 1)) != 0){
        return NULL; /* alignment must be a power of two */
    }
    if(alignment > (size_t)ECCLES_RT_ALIGNMENT){
        /* can't guarantee more than the built-in per-allocation alignment
           without per-allocation offset bookkeeping this library doesn't
           keep - see the note on this function in eccles_rtmem.h */
        return NULL;
    }
    return eccles_rt_malloc(size); /* already aligned to ECCLES_RT_ALIGNMENT unconditionally */
}

bool eccles_rt_reset(void){
    ECCLES_RT_LOCK_STATE_T lockState;

    if(!eccles_rt_initialized || !ECCLES_RT_MUTEX_VALID(eccles_rt_lock)) return false;
#if defined(ECCLES_RT_MEM_USE_HEAP)
    if(eccles_rt_pool == NULL || eccles_rt_reg == NULL) return false;
#endif

    ECCLES_RT_LOCK(eccles_rt_lock, lockState);
    {
        uint16_t k;
        for(k = 0; k < ECCLES_RT_TOTAL_BLOCKS; k++) eccles_rt_reg[k] = 0;
    }
    eccles_rt__bm_clear_all();
    eccles_rt_pools[POOL_A].cursor     = ECCLES_RT__REGBASE_A;
    eccles_rt_pools[POOL_A].usedBlocks = 0;
    eccles_rt_pools[POOL_B].cursor     = ECCLES_RT__REGBASE_B;
    eccles_rt_pools[POOL_B].usedBlocks = 0;
    eccles_rt_pools[POOL_C].cursor     = ECCLES_RT__REGBASE_C;
    eccles_rt_pools[POOL_C].usedBlocks = 0;
    ECCLES_RT_UNLOCK(eccles_rt_lock, lockState);

    return true;
}

eccles_rt_stats_t eccles_rt_get_stats(void){
    eccles_rt_stats_t st;
    ECCLES_RT_LOCK_STATE_T lockState;
    st.usedA = st.freeA = st.usedB = st.freeB = st.usedC = st.freeC = 0;

    if(!eccles_rt_initialized || !ECCLES_RT_MUTEX_VALID(eccles_rt_lock)) return st;

    /* O(1): read the running usedBlocks counters getFree/freeBuffer keep
       in sync, instead of rescanning the whole registry (up to 255
       entries) under the lock on every call - this used to mean even a
       cheap, frequent stats poll paid for the full O(N) scan while
       holding the lock the whole time, which is exactly the kind of
       thing you don't want blocking another task's malloc/free. */
    ECCLES_RT_LOCK(eccles_rt_lock, lockState);
    st.usedA = eccles_rt_pools[POOL_A].usedBlocks;
    st.usedB = eccles_rt_pools[POOL_B].usedBlocks;
    st.usedC = eccles_rt_pools[POOL_C].usedBlocks;
    ECCLES_RT_UNLOCK(eccles_rt_lock, lockState);

    st.freeA = (uint8_t)(ECCLES_RT_POOL_A_COUNT - st.usedA);
    st.freeB = (uint8_t)(ECCLES_RT_POOL_B_COUNT - st.usedB);
    st.freeC = (uint8_t)(ECCLES_RT_POOL_C_COUNT - st.usedC);
    return st;
}
