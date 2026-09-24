/*
    ECCLES RTLib - RuntimeMemory implementation
    Portable plain-C rewrite: same pool-allocator design as the original
    ESP-IDF version, generalized so pool sizes/counts fall out of
    ECCLES_RT_MEM_SIZE (and the block-size/percent knobs) instead of being
    hardcoded, and locking/logging are picked per-platform instead of
    assuming FreeRTOS is always available.
*/

#include "eccles_rtmem.h"

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

#elif defined(__arm__) || defined(__ARM_ARCH) || defined(__CORTEX_M)
  /* generic Cortex-M bare metal (no RTOS detected above): STM32 (any
     line not already matched), SAMD, nRF52 without Zephyr/mbed's own
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

#define ECCLES_RT__POOL_BYTES \
    ((size_t)ECCLES_RT_POOL_A_COUNT * ECCLES_RT_BLOCK_A_SIZE + \
     (size_t)ECCLES_RT_POOL_B_COUNT * ECCLES_RT_BLOCK_B_SIZE + \
     (size_t)ECCLES_RT_POOL_C_COUNT * ECCLES_RT_BLOCK_C_SIZE)

#define ECCLES_RT__OFFSET_A ((size_t)0)
#define ECCLES_RT__OFFSET_B (ECCLES_RT__OFFSET_A + (size_t)ECCLES_RT_POOL_A_COUNT * ECCLES_RT_BLOCK_A_SIZE)
#define ECCLES_RT__OFFSET_C (ECCLES_RT__OFFSET_B + (size_t)ECCLES_RT_POOL_B_COUNT * ECCLES_RT_BLOCK_B_SIZE)

#define ECCLES_RT__REGBASE_A (0)
#define ECCLES_RT__REGBASE_B (ECCLES_RT_POOL_A_COUNT)
#define ECCLES_RT__REGBASE_C (ECCLES_RT_POOL_A_COUNT + ECCLES_RT_POOL_B_COUNT)

/* every pointer eccles_rt_malloc() hands out is aligned to
   ECCLES_RT_ALIGNMENT bytes: automatic in heap mode (malloc() already
   guarantees max alignment), applied explicitly here in static mode via
   a GCC/Clang alignment attribute. On a non-GCC/Clang toolchain in
   static mode, align eccles_rt_pool yourself with your compiler's own
   attribute/pragma if you need to cast returned pointers to wider types. */
#if defined(ECCLES_RT_MEM_USE_HEAP)
  #include <stdlib.h>
  static uint8_t *eccles_rt_pool = NULL;
  static uint8_t *eccles_rt_reg  = NULL;
#elif defined(__GNUC__) || defined(__clang__)
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
} eccles_rt_pool_t;

static eccles_rt_pool_t eccles_rt_pools[3];
static ECCLES_RT_MUTEX_T eccles_rt_lock;

/* pool indices, in the fallback order eccles_rt_malloc uses for oversized requests */
#define POOL_A 0
#define POOL_B 1
#define POOL_C 2

static uint8_t eccles_rt__log2_exact(size_t v){
    uint8_t shift = 0;
    if(v == 0) return 0xFF;
    while((v & 1u) == 0u){ v >>= 1; shift++; }
    return (v == 1u) ? shift : 0xFF; /* 0xFF => not a power of two */
}

/* ===================================================================== *
 *  allocation core
 * ===================================================================== */

static uint8_t* eccles_rt_getFree(uint8_t size, uint8_t poolIdx){
    eccles_rt_pool_t *p = &eccles_rt_pools[poolIdx];
    uint8_t rsb = p->regBase;
    uint8_t rsz = p->count;
    uint8_t *bpt = NULL;
    uint8_t step, i, e;

    /* a request whose size doesn't fit in this pool at all can never be satisfied here */
    if(size == 0 || size > rsz){
        ECCLES_RT_LOG_LINE("eccles_rtmem: request exceeds this pool's total block count, not just fragmentation");
        return NULL;
    }

    /* cursor runs circular across this pool's own slice of the registry.
       NOTE: the index arithmetic here must happen in a type wide enough
       to hold cursor+step without wrapping BEFORE the boundary check runs
       - p->cursor and step are both uint8_t, and when a pool's absolute
       end index (rsb+rsz) is close to 256, cursor+step can itself
       overflow 255 before reaching rsb+rsz, so truncating to uint8_t
       first (as this used to) can silently produce an index that lands
       in a totally different pool's territory instead of wrapping back
       to the start of this one. Doing the add/compare in uint16_t and
       only narrowing to uint8_t afterward avoids that. */
    for(step = 0; step < rsz; step++){
        uint16_t iw = (uint16_t)p->cursor + (uint16_t)step;
        if(iw >= (uint16_t)(rsb + rsz)) iw = (uint16_t)(iw - rsz);
        i = (uint8_t)iw;

        /* don't let a multi-block request scan or land past this pool's own boundary */
        if((uint16_t)i + (uint16_t)size > (uint16_t)(rsb + rsz)) continue;

        if(eccles_rt_reg[i] == 0){
            bool free_run = true;
            for(e = i; e < (uint8_t)(i + size); e++){
                if(eccles_rt_reg[e] != 0){ free_run = false; break; }
            }
            if(free_run){
                size_t byteOffset;
                if(p->shift != 0xFF){
                    byteOffset = ((size_t)(i - rsb) << p->shift) + p->offset;
                } else {
                    byteOffset = ((size_t)(i - rsb) * p->blockSize) + p->offset;
                }
                bpt = &eccles_rt_pool[byteOffset];

                eccles_rt_reg[i] = size;
                {
                    uint8_t iu;
                    for(iu = (uint8_t)(i + 1); iu < (uint8_t)(i + size); iu++){
                        eccles_rt_reg[iu] = (uint8_t)ECCLES_RT_CONT_MARK;
                    }
                }

                {
                    uint8_t next = (uint8_t)(i + size);
                    if(next >= rsb + rsz) next = (uint8_t)(next - rsz);
                    p->cursor = next;
                }
                break;
            }
        }
    }

    if(bpt == NULL){
        ECCLES_RT_LOG_LINE("eccles_rtmem: size fits this pool but no contiguous run of free blocks was found");
    }
    return bpt;
}

static eccles_rt_free_status_t eccles_rt_freeBuffer(uint8_t* buffer){
    /* pointer arithmetic/comparison against an arbitrary (possibly
       invalid) pointer is only well-defined when both pointers are known
       to be inside the same array - buffer might not be, so route
       everything through uintptr_t integer comparisons instead of
       comparing/subtracting the uint8_t* pointers directly */
    uintptr_t base = (uintptr_t)eccles_rt_pool;
    uintptr_t addr = (uintptr_t)buffer;
    size_t oft;
    eccles_rt_pool_t *p = NULL;
    uint8_t rind, mb, i;

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

    /* reject pointers that don't land exactly on a block boundary */
    if(p->shift != 0xFF){
        if(((oft - p->offset) & (p->blockSize - 1)) != 0){
            ECCLES_RT_LOG_LINE("eccles_rtmem: eccles_rt_free pointer is not a block start, ignoring");
            return ECCLES_RT_FREE_MISALIGNED;
        }
        rind = (uint8_t)(((oft - p->offset) >> p->shift) + p->regBase);
    } else {
        if(((oft - p->offset) % p->blockSize) != 0){
            ECCLES_RT_LOG_LINE("eccles_rtmem: eccles_rt_free pointer is not a block start, ignoring");
            return ECCLES_RT_FREE_MISALIGNED;
        }
        rind = (uint8_t)(((oft - p->offset) / p->blockSize) + p->regBase);
    }

    mb = eccles_rt_reg[rind];

    /* reject double frees and mid-run pointers instead of corrupting the registry */
    if(mb == 0){
        ECCLES_RT_LOG_LINE("eccles_rtmem: eccles_rt_free buffer already free, ignoring double free");
        return ECCLES_RT_FREE_DOUBLE_FREE;
    }
    if(mb == (uint8_t)ECCLES_RT_CONT_MARK){
        ECCLES_RT_LOG_LINE("eccles_rtmem: eccles_rt_free pointer is mid-run, not a block start, ignoring");
        return ECCLES_RT_FREE_MID_RUN;
    }

    for(i = rind; i < (uint8_t)(rind + mb); i++){
        eccles_rt_reg[i] = 0;
    }

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
    eccles_rt_pool = (uint8_t*)malloc(ECCLES_RT__POOL_BYTES);
    eccles_rt_reg  = (uint8_t*)malloc(ECCLES_RT_TOTAL_BLOCKS);
    if(eccles_rt_pool == NULL || eccles_rt_reg == NULL){
        ECCLES_RT_LOG_LINE("eccles_rtmem: heap allocation failed in eccles_rtmem_init");
        free(eccles_rt_pool); eccles_rt_pool = NULL;
        free(eccles_rt_reg);  eccles_rt_reg  = NULL;
        return false; /* stays uninitialized - call again later to retry */
    }
    {
        uint16_t k;
        for(k = 0; k < ECCLES_RT_TOTAL_BLOCKS; k++) eccles_rt_reg[k] = 0;
    }
#endif

    eccles_rt_pools[POOL_A].offset    = ECCLES_RT__OFFSET_A;
    eccles_rt_pools[POOL_A].blockSize = ECCLES_RT_BLOCK_A_SIZE;
    eccles_rt_pools[POOL_A].count     = ECCLES_RT_POOL_A_COUNT;
    eccles_rt_pools[POOL_A].regBase   = ECCLES_RT__REGBASE_A;
    eccles_rt_pools[POOL_A].cursor    = ECCLES_RT__REGBASE_A;
    eccles_rt_pools[POOL_A].shift     = eccles_rt__log2_exact(ECCLES_RT_BLOCK_A_SIZE);

    eccles_rt_pools[POOL_B].offset    = ECCLES_RT__OFFSET_B;
    eccles_rt_pools[POOL_B].blockSize = ECCLES_RT_BLOCK_B_SIZE;
    eccles_rt_pools[POOL_B].count     = ECCLES_RT_POOL_B_COUNT;
    eccles_rt_pools[POOL_B].regBase   = ECCLES_RT__REGBASE_B;
    eccles_rt_pools[POOL_B].cursor    = ECCLES_RT__REGBASE_B;
    eccles_rt_pools[POOL_B].shift     = eccles_rt__log2_exact(ECCLES_RT_BLOCK_B_SIZE);

    eccles_rt_pools[POOL_C].offset    = ECCLES_RT__OFFSET_C;
    eccles_rt_pools[POOL_C].blockSize = ECCLES_RT_BLOCK_C_SIZE;
    eccles_rt_pools[POOL_C].count     = ECCLES_RT_POOL_C_COUNT;
    eccles_rt_pools[POOL_C].regBase   = ECCLES_RT__REGBASE_C;
    eccles_rt_pools[POOL_C].cursor    = ECCLES_RT__REGBASE_C;
    eccles_rt_pools[POOL_C].shift     = eccles_rt__log2_exact(ECCLES_RT_BLOCK_C_SIZE);

    if(eccles_rt_pools[POOL_A].shift == 0xFF || eccles_rt_pools[POOL_B].shift == 0xFF || eccles_rt_pools[POOL_C].shift == 0xFF){
        ECCLES_RT_LOG_LINE("eccles_rtmem: a block size is not a power of two, falling back to slower division math for that pool");
    }

    eccles_rt_lock = (ECCLES_RT_MUTEX_T)ECCLES_RT_LOCK_INIT();
    (void)lockState; /* only declared for type-checking ECCLES_RT_LOCK_STATE_T exists; unused here */
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

    if(size <= ECCLES_RT_BLOCK_A_SIZE){
        result = eccles_rt_getFree(1, POOL_A);
    } else if(size <= ECCLES_RT_BLOCK_B_SIZE){
        result = eccles_rt_getFree(1, POOL_B);
    } else if(size <= ECCLES_RT_BLOCK_C_SIZE){
        result = eccles_rt_getFree(1, POOL_C);
    } else {
        /* bigger than one C block: try a contiguous multi-block run,
           starting with the biggest block size and falling back to
           smaller ones if that pool is too fragmented to satisfy it.
           This never combines blocks from two different pools into one
           allocation - each attempt is single-class only. */
        static const uint8_t order[3] = { POOL_C, POOL_B, POOL_A };
        uint8_t oi;
        for(oi = 0; oi < 3 && result == NULL; oi++){
            uint8_t poolIdx = order[oi];
            size_t bs = eccles_rt_pools[poolIdx].blockSize;
            size_t need = size / bs + ((size % bs) ? 1 : 0);
            if(need <= 255){
                result = eccles_rt_getFree((uint8_t)need, poolIdx);
            }
        }
        if(result == NULL){
            ECCLES_RT_LOG_LINE("eccles_rtmem: buffer allocation failed, no pool had a large enough contiguous run free");
        }
    }

    ECCLES_RT_UNLOCK(eccles_rt_lock, lockState);
    return result;
}

eccles_rt_free_status_t eccles_rt_free(uint8_t* buffer){
    eccles_rt_free_status_t status;
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
    return status;
}

eccles_rt_stats_t eccles_rt_get_stats(void){
    eccles_rt_stats_t st;
    ECCLES_RT_LOCK_STATE_T lockState;
    uint8_t i;
    st.usedA = st.freeA = st.usedB = st.freeB = st.usedC = st.freeC = 0;

    if(!eccles_rt_initialized || !ECCLES_RT_MUTEX_VALID(eccles_rt_lock)) return st;
    ECCLES_RT_LOCK(eccles_rt_lock, lockState);

    for(i = 0; i < ECCLES_RT_POOL_A_COUNT; i++){
        if(eccles_rt_reg[i] == 0) st.freeA++; else st.usedA++;
    }
    for(i = ECCLES_RT_POOL_A_COUNT; i < ECCLES_RT_POOL_A_COUNT + ECCLES_RT_POOL_B_COUNT; i++){
        if(eccles_rt_reg[i] == 0) st.freeB++; else st.usedB++;
    }
    for(i = ECCLES_RT_POOL_A_COUNT + ECCLES_RT_POOL_B_COUNT; i < ECCLES_RT_TOTAL_BLOCKS; i++){
        if(eccles_rt_reg[i] == 0) st.freeC++; else st.usedC++;
    }

    ECCLES_RT_UNLOCK(eccles_rt_lock, lockState);
    return st;
}
