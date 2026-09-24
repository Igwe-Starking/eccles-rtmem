/*
    ECCLES RTLib - RuntimeMemory (portable, plain C)
    --------------------------------------------------
    A fixed-pool allocator meant to replace malloc()/free() on microcontrollers
    (Arduino / ESP-IDF / STM32 / bare-metal or RTOS) where frequent heap churn
    (radio buffers, audio queues, protocol frames, etc.) risks fragmentation
    on devices that may only have a few KB of RAM to begin with.

    Design, in short:
      - one big block of memory (static array or heap block, your choice)
        split into 3 pools of fixed block sizes: A (small) / B (medium) / C (large)
      - eccles_rt_malloc() picks the smallest pool that fits, or for
        requests bigger than one C block, walks a chain of contiguous
        blocks (C, then B, then A as fallbacks if a pool is fragmented) -
        each attempt is single-class only; it never combines blocks from
        two different pools into one allocation (see "Positioning" below)
      - eccles_rt_free() validates the pointer before touching any
        bookkeeping, and tells you why if it rejected one
      - everything lives in fixed-size arrays sized from compile-time
        constants, no dynamic growth, no STL/heap dependency required

    Positioning: this is a deterministic, bounded FIXED-POOL allocator,
    not a general-purpose malloc() replacement. It trades some memory
    efficiency (fixed block classes mean internal fragmentation - a
    33-byte request against a 64-byte block class wastes 31 bytes) for
    bounded RAM usage, a fixed pool capacity decided at compile time, and
    no dependence on a conventional heap. Because A/B/C are independent
    pools, a request can fail even when the *total* free memory across all
    three pools would have been enough - free bytes in one pool cannot
    satisfy a request that belongs to a different pool's class, and one
    single-class run is all any one allocation ever gets.

    ---------------------------------------------------------------------
    CONFIGURATION (define these BEFORE including this header, if you want
    something other than the defaults)
    ---------------------------------------------------------------------

    ECCLES_RT_MEM_SIZE
        Total budget, in bytes, for the three pools combined.
        Default: 1024 (1 KB) on MCU families known to be RAM-constrained
        (8-bit AVR, MSP430, STM8, 8-bit PIC, 8051, and Cortex-M0/M0+ entry
        STM32 lines), else 20480 (20 KB) on everything else (ESP32/ESP8266,
        bigger STM32 lines, SAMD, RP2040, nRF52, PIC32, desktop test
        builds, ...). See the platform table further down.

        IMPORTANT: this is a family-level guess, not a per-chip one. An
        MCU family can span a huge RAM range (a "big" STM32F1 part might
        have 4 KB or 128 KB depending on the exact model) - the auto-
        detected number can be outright wrong, even dangerously so, for
        your specific chip. Treat the auto-default as a convenience for
        prototyping/examples only; for anything you intend to ship, set
        ECCLES_RT_MEM_SIZE explicitly to a number you've checked against
        your part's actual RAM. On GCC/Clang, leaving it on auto-detect
        triggers a `#warning` reminding you of this.

    ECCLES_RT_MEM_USE_HEAP
        If defined, the pool and its bookkeeping registry are malloc()'d
        once inside eccles_rtmem_init() instead of living as static arrays.
        Leave undefined (the default) to keep everything static (.bss),
        which is usually what you want on a microcontroller: the size is
        known at compile time either way, this only changes *where* the
        bytes live and whether you pay for them if init is never called.

    ECCLES_RT_ALIGNMENT
        Byte alignment guaranteed for every pointer eccles_rt_malloc()
        returns, so you can safely cast it to another object type (e.g.
        `uint32_t *p = (uint32_t*)eccles_rt_malloc(sizeof *p);`). Default:
        8. In heap mode this is automatic (malloc() already guarantees
        max-alignment); in static mode it's applied via a GCC/Clang
        `__attribute__((aligned(...)))` on the pool array - if you're on a
        toolchain that isn't GCC/Clang-family (classic ARMCC5, IAR without
        the extension) you'll need to align the array yourself, e.g. via
        that toolchain's own alignment pragma/attribute.

    ECCLES_RT_BLOCK_A_SIZE / _B_SIZE / _C_SIZE
        Block size, in bytes, of each pool. Must be a power of two, and
        A <= B <= C (both checked at compile time).
        Default: 16 / 32 / 64 if ECCLES_RT_MEM_SIZE <= 4096, else 64 / 128 / 256.

    ECCLES_RT_POOL_A_PERCENT / _B_PERCENT
        Percentage of ECCLES_RT_MEM_SIZE handed to pool A and pool B; pool C
        gets whatever remains. Default: 25 / 25 (pool C gets ~50%), matching
        the ratio the original fixed-size version shipped with. Their sum
        can't exceed 100 (checked at compile time).

    ECCLES_RT_DEBUG
        Define to enable ECCLES_RT_LOG_LINE() calls on allocation failures /
        rejected frees. You can supply your own ECCLES_RT_LOG_LINE(msg)
        macro (e.g. wrapping Serial.println on Arduino) before including
        this header; otherwise a printf() fallback is used.

    ECCLES_RT_MEM_NO_LOCK
        Define to strip out all locking entirely - no mutex, no interrupt
        masking, nothing. Use this only if you know eccles_rt_malloc/
        eccles_rt_free will ever run from a single thread/task/ISR context,
        or if you're doing your own coarser-grained locking around whole
        sequences of calls yourself. This always wins over every platform
        auto-detection below, and over a manual
        ECCLES_RT_LOCK_INIT/_LOCK/_UNLOCK override.

    ECCLES_RT_LOCK_INIT() / ECCLES_RT_LOCK(m, st) / ECCLES_RT_UNLOCK(m, st) / ECCLES_RT_MUTEX_T / ECCLES_RT_LOCK_STATE_T
        Define all five together to plug in your own mutex/critical-section
        (any RTOS, any chip). `m` is the single shared mutex object
        (ECCLES_RT_MUTEX_T); `st` is a variable of type
        ECCLES_RT_LOCK_STATE_T declared fresh at each call site (not a
        shared static) - this is what makes interrupt-mask-style locking
        actually reentrant: each call saves/restores its own previous
        interrupt state instead of every call site stomping one shared
        variable. If your backend doesn't need saved state (a real RTOS
        mutex, for instance), just ignore `st` in your macros and give
        ECCLES_RT_LOCK_STATE_T a throwaway type like uint8_t.

        NOTE: this is a breaking change from an earlier version of this
        header, which only took `m` (no `st`). If you had a manual
        override written against that version, add the `st` parameter
        (and ECCLES_RT_LOCK_STATE_T) before upgrading.

        If you don't override these, and ECCLES_RT_MEM_NO_LOCK isn't
        defined either, eccles_rtmem.c auto-detects, in this order:
          1) Zephyr           - k_mutex, if __ZEPHYR__ looks present
          2) FreeRTOS          - a real semaphore, if FreeRTOS looks present
                                  (ESP-IDF, ESP32 Arduino core, or you
                                  #define ECCLES_RT_USE_FREERTOS yourself)
          3) CMSIS-RTOS2/RTX5  - osMutex, if cmsis_os2.h is reachable
                                  (typical of STM32CubeMX-generated projects)
          4) Pico SDK          - critical_section_t, on RP2040/RP2350
                                  (needed, not just nice-to-have: RP2040 is
                                  dual-core, so a plain interrupt-mask
                                  critical section on one core would NOT
                                  block the other core from touching the
                                  pools at the same time)
          5) AVR               - cli()/sei(), saving/restoring SREG in a
                                  call-site-local variable, if __AVR__ is
                                  defined (plain Uno/Nano/Mega - no RTOS
                                  underneath)
          6) MSP430            - __disable_interrupt()/__enable_interrupt(),
                                  saving/restoring interrupt state in a
                                  call-site-local variable, if __MSP430__ is
                                  defined
          7) Microchip XC16/XC32 - __builtin_disable_interrupts(), saving/
                                  restoring ISR state in a call-site-local
                                  variable, on dsPIC/PIC24/PIC32
          8) generic Cortex-M  - PRIMASK save/disable/restore in a call-
                                  site-local variable, using whichever
                                  intrinsic/asm your toolchain wants (IAR /
                                  Keil-ARMCC / GCC-Clang), for any ARM
                                  target not already matched above (covers
                                  most STM32 lines, SAMD, nRF52 without an
                                  RTOS, etc.)
          9) no-op, otherwise  - assumes a single-threaded, non-preemptive
                                  bare-metal build. If that's wrong for your
                                  target (some other RTOS, an 8-bit chip
                                  without its own case above, ...), either
                                  define the five macros yourself, or use
                                  your compiler's interrupt-disable/enable
                                  pair the same way the AVR/MSP430 cases do.

        Reentrancy/ISR notes for whichever backend ends up active:
          - eccles_rt_malloc/_free/_get_stats are NOT designed to be called
            re-entrantly from within themselves (they never do this
            internally, and you shouldn't either).
          - interrupt-masking backends (AVR/MSP430/XC16/XC32/generic
            Cortex-M) are ISR-safe for both allocation and free: calling
            from an ISR while the mainline holds the lock cannot happen,
            because interrupts are masked for the whole locked section.
          - RTOS-mutex backends (FreeRTOS/Zephyr/CMSIS-RTOS2) are NOT
            ISR-safe by default - their blocking lock/unlock calls aren't
            meant to run in interrupt context. Don't call eccles_rt_malloc/
            _free from an ISR when one of these is the active backend
            unless you've swapped in that RTOS's FromISR-style API
            yourself via a manual override.
          - expected critical-section duration is short and bounded: one
            scan of at most one pool's blocks (or, for an oversized
            request, up to three single-pool scans in the fallback chain) -
            see "Allocation cost" below for what that scan actually costs.

    Allocation cost: eccles_rt_malloc() scans forward from a per-pool
    cursor looking for a free run, so the worst case is proportional to
    that pool's block count (bounded by ECCLES_RT_POOL_A/B/C_COUNT, i.e.
    by your configured budget - not unbounded). This is "bounded" and
    "predictable for a fixed configuration", not a hard real-time
    constant-time guarantee: how long a given call takes still depends on
    how fragmented that pool is at that moment. No worst-case timing has
    been measured on real hardware for this release - treat this as
    bounded-but-not-formally-real-time until you've benchmarked it on your
    own target.
*/

/* ---------------------------------------------------------------------
   platform reference: what this header assumes about RAM and locking for
   a range of common MCU families. "auto" locking means one of the cases
   above kicks in without you doing anything; "manual" means you'll want
   to supply your own ECCLES_RT_LOCK_INIT/_LOCK/_UNLOCK/_MUTEX_T.
   ---------------------------------------------------------------------
   family                         default budget   locking
   ---------------------------------------------------------------------
   AVR (Uno/Nano/Mega/ATtiny)     1 KB             auto (cli/sei)
   MSP430                         1 KB             auto (disable/enable)
   STM8                           1 KB             manual (no portable
                                                     builtin across
                                                     Cosmic/IAR/SDCC)
   8-bit PIC (XC8)                1 KB             manual (register bits
                                                     differ per family)
   8051 (Keil C51 / SDCC mcs51)   1 KB             manual (EA bit, syntax
                                                     differs per toolchain)
   STM32 F0/G0/L0/C0 (Cortex-M0)  1 KB             auto (Cortex-M PRIMASK)
   STM32 other lines, no RTOS     20 KB            auto (Cortex-M PRIMASK)
   STM32 + FreeRTOS/CMSIS-RTOS2   20 KB            auto (their mutex)
   ESP32 / ESP8266 (Arduino/IDF)  20 KB            auto on ESP32/IDF
                                                     (FreeRTOS); ESP8266
                                                     Arduino core has none
                                                     exposed, falls to no-op
   RP2040 / RP2350 (Pico SDK)     20 KB            auto (critical_section_t
                                                     - safe across both cores)
   SAMD21 / SAMD51                20 KB            auto (Cortex-M PRIMASK)
   nRF52 (bare, mbed, or Zephyr)  20 KB            auto (Zephyr k_mutex, or
                                                     Cortex-M PRIMASK if bare)
   PIC32 (XC32)                   20 KB            auto (ISR-state builtin)
   --------------------------------------------------------------------- */


#ifndef ECCLES_RTMEM_H
#define ECCLES_RTMEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
    IMPORTANT: every translation unit that includes this header - your
    sketch/app code AND eccles_rtmem.c itself - must see the exact same
    configuration macros, since pool sizes are baked in at preprocess
    time. The safe ways to set them:

      1) compiler command-line defines (-DECCLES_RT_MEM_SIZE=4096, etc:
         PlatformIO's build_flags, CMake target_compile_definitions, ...)
         these apply to every .c/.cpp file automatically.

      2) drop a file named "eccles_rtmem_config.h" next to your sources
         with your #define's in it (nothing else needed) - if your
         compiler supports __has_include (GCC/Clang/AVR-GCC/ARMCC6/IAR
         all do), it is picked up automatically by every file that
         includes eccles_rtmem.h, including eccles_rtmem.c.

    What will NOT work: #define'ing these directly above an #include
    "eccles_rtmem.h" in just your sketch file. eccles_rtmem.c has its own
    #include of this header and won't see that #define, so the two files
    would disagree about how big the pools are.
*/
#if defined(__has_include)
  #if __has_include("eccles_rtmem_config.h")
    #include "eccles_rtmem_config.h"
  #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== *
 *  memory budget - detect RAM-constrained MCU families and default to a
 *  1 KB pool on them; everything else defaults to 20 KB. Override with
 *  your own ECCLES_RT_MEM_SIZE any time this guess doesn't fit your part
 *  (see the IMPORTANT note above - this is a family-level guess, not a
 *  per-chip one).
 * ===================================================================== */

#ifndef ECCLES_RT_MEM_SIZE
  #if defined(__GNUC__) || defined(__clang__)
    #warning "eccles_rtmem: ECCLES_RT_MEM_SIZE not set, guessing a budget from your MCU family. Set it explicitly for anything you intend to ship - see the ECCLES_RT_MEM_SIZE note in eccles_rtmem.h."
  #endif
  #if defined(__AVR__)                                       /* Arduino Uno/Nano/Mega, ATtiny, ... */ \
   || defined(__MSP430__)                                     /* TI MSP430 */ \
   || defined(__STM8__)                                       /* STMicro STM8 */ \
   || defined(__XC8__) || defined(_PIC12) || defined(_PIC16) || defined(_PIC18) /* 8-bit Microchip PIC */ \
   || defined(__C51__) || defined(__CX51__)                   /* Keil C51 (8051) */ \
   || (defined(__SDCC) && defined(__SDCC_mcs51))               /* SDCC targeting 8051 */ \
   || defined(STM32F0) || defined(STM32G0) || defined(STM32L0) || defined(STM32C0) /* entry-level Cortex-M0/M0+ STM32 */
    #define ECCLES_RT_MEM_SIZE 1024ul     /* 1 KB on RAM-constrained MCUs */
  #else
    #define ECCLES_RT_MEM_SIZE (20ul*1024ul) /* 20 KB: ESP32/ESP8266, bigger STM32 lines, SAMD, RP2040, nRF52, PIC32, desktop test builds, ... */
  #endif
#endif

#if (ECCLES_RT_MEM_SIZE) == 0
  #error "eccles_rtmem: ECCLES_RT_MEM_SIZE must be nonzero."
#endif

/* ===================================================================== *
 *  block sizes (must be powers of two, and A <= B <= C)
 * ===================================================================== */

#ifndef ECCLES_RT_BLOCK_A_SIZE
  #if (ECCLES_RT_MEM_SIZE) <= 4096ul
    #define ECCLES_RT_BLOCK_A_SIZE 16ul
    #define ECCLES_RT_BLOCK_B_SIZE 32ul
    #define ECCLES_RT_BLOCK_C_SIZE 64ul
  #else
    #define ECCLES_RT_BLOCK_A_SIZE 64ul
    #define ECCLES_RT_BLOCK_B_SIZE 128ul
    #define ECCLES_RT_BLOCK_C_SIZE 256ul
  #endif
#endif
#ifndef ECCLES_RT_BLOCK_B_SIZE
  #define ECCLES_RT_BLOCK_B_SIZE (ECCLES_RT_BLOCK_A_SIZE * 2ul)
#endif
#ifndef ECCLES_RT_BLOCK_C_SIZE
  #define ECCLES_RT_BLOCK_C_SIZE (ECCLES_RT_BLOCK_B_SIZE * 2ul)
#endif

#if (ECCLES_RT_BLOCK_A_SIZE) == 0 || (ECCLES_RT_BLOCK_B_SIZE) == 0 || (ECCLES_RT_BLOCK_C_SIZE) == 0
  #error "eccles_rtmem: ECCLES_RT_BLOCK_A/B/C_SIZE must all be nonzero."
#endif
#if !((ECCLES_RT_BLOCK_A_SIZE) <= (ECCLES_RT_BLOCK_B_SIZE) && (ECCLES_RT_BLOCK_B_SIZE) <= (ECCLES_RT_BLOCK_C_SIZE))
  #error "eccles_rtmem: block sizes must satisfy A <= B <= C - eccles_rt_malloc()'s size routing assumes that ordering."
#endif

/* ===================================================================== *
 *  pool split (A / B percent of the budget, C gets the remainder)
 * ===================================================================== */

#ifndef ECCLES_RT_POOL_A_PERCENT
  #define ECCLES_RT_POOL_A_PERCENT 25ul
#endif
#ifndef ECCLES_RT_POOL_B_PERCENT
  #define ECCLES_RT_POOL_B_PERCENT 25ul
#endif

#if (ECCLES_RT_POOL_A_PERCENT) + (ECCLES_RT_POOL_B_PERCENT) > 100ul
  #error "eccles_rtmem: ECCLES_RT_POOL_A_PERCENT + ECCLES_RT_POOL_B_PERCENT must not exceed 100."
#endif

#define ECCLES_RT__BYTES_A ((ECCLES_RT_MEM_SIZE * ECCLES_RT_POOL_A_PERCENT) / 100ul)
#define ECCLES_RT__BYTES_B ((ECCLES_RT_MEM_SIZE * ECCLES_RT_POOL_B_PERCENT) / 100ul)
#define ECCLES_RT__BYTES_C (ECCLES_RT_MEM_SIZE - ECCLES_RT__BYTES_A - ECCLES_RT__BYTES_B)

#define ECCLES_RT__RAWCOUNT_A (ECCLES_RT__BYTES_A / ECCLES_RT_BLOCK_A_SIZE)
#define ECCLES_RT__RAWCOUNT_B (ECCLES_RT__BYTES_B / ECCLES_RT_BLOCK_B_SIZE)
#define ECCLES_RT__RAWCOUNT_C (ECCLES_RT__BYTES_C / ECCLES_RT_BLOCK_C_SIZE)

/* each pool needs >=1 block to exist at all, and <=254 blocks: block
   *count* doubles as the "run length" byte in the bookkeeping registry,
   and 255 (CONT_MARK) is reserved to mean "middle of a run", so a pool
   can never legally hold 255 blocks */
#define ECCLES_RT__CLAMP(x) ((x) < 1ul ? 1ul : ((x) > 254ul ? 254ul : (x)))

#define ECCLES_RT_POOL_A_COUNT ECCLES_RT__CLAMP(ECCLES_RT__RAWCOUNT_A)
#define ECCLES_RT_POOL_B_COUNT ECCLES_RT__CLAMP(ECCLES_RT__RAWCOUNT_B)
#define ECCLES_RT_POOL_C_COUNT ECCLES_RT__CLAMP(ECCLES_RT__RAWCOUNT_C)

#define ECCLES_RT_TOTAL_BLOCKS (ECCLES_RT_POOL_A_COUNT + ECCLES_RT_POOL_B_COUNT + ECCLES_RT_POOL_C_COUNT)

/* the registry index type is uint8_t on purpose (see eccles_rtmem.c) to
   keep bookkeeping RAM minimal on weak devices, so the combined block
   count across all three pools has to fit in one byte */
#if ECCLES_RT_TOTAL_BLOCKS > 255
  #error "eccles_rtmem: total block count (A+B+C) exceeds 255. Raise your block sizes, " \
         "lower ECCLES_RT_MEM_SIZE, or adjust ECCLES_RT_POOL_A_PERCENT/_B_PERCENT."
#endif

#ifndef ECCLES_RT_ALIGNMENT
  #define ECCLES_RT_ALIGNMENT 8
#endif

/* ===================================================================== *
 *  public API
 * ===================================================================== */

typedef struct {
    uint8_t usedA, freeA;   /* pool A: ECCLES_RT_BLOCK_A_SIZE-byte blocks */
    uint8_t usedB, freeB;   /* pool B: ECCLES_RT_BLOCK_B_SIZE-byte blocks */
    uint8_t usedC, freeC;   /* pool C: ECCLES_RT_BLOCK_C_SIZE-byte blocks */
} eccles_rt_stats_t;

/* what eccles_rt_free() actually did, instead of silently ignoring a
   rejected pointer */
typedef enum {
    ECCLES_RT_FREE_OK = 0,        /* freed successfully */
    ECCLES_RT_FREE_NULL,          /* buffer was NULL - harmless no-op */
    ECCLES_RT_FREE_NOT_READY,     /* eccles_rtmem_init() hasn't run (or failed) yet */
    ECCLES_RT_FREE_OUT_OF_RANGE,  /* pointer isn't inside any pool this allocator owns */
    ECCLES_RT_FREE_MISALIGNED,    /* inside a pool, but not on a block boundary */
    ECCLES_RT_FREE_DOUBLE_FREE,   /* that block is already marked free */
    ECCLES_RT_FREE_MID_RUN        /* pointer into the middle of a multi-block allocation, not its start */
} eccles_rt_free_status_t;

/* eccles_rtmem_init() must be called once (e.g. from setup()/app_main())
   before any eccles_rt_malloc/_free/_get_stats call. Safe to call more
   than once: every call after the first is a no-op that just returns
   true again, so you can't accidentally re-malloc() a fresh pool out from
   under buffers you've already handed out. Returns false only if
   ECCLES_RT_MEM_USE_HEAP is defined and the underlying malloc() failed -
   in that case eccles_rt_malloc/_free/_get_stats stay inert (NULL/no-op/
   zeroed) rather than touching unallocated memory, and you can call
   eccles_rtmem_init() again later to retry. */
bool eccles_rtmem_init(void);

#ifndef ECCLES_RT_NO_DEPRECATED_ALIASES
/* deprecated aliases for the pre-review names - define
   ECCLES_RT_NO_DEPRECATED_ALIASES before including this header if you
   don't want them. e_free's old callers see it return a
   eccles_rt_free_status_t now instead of void; ignoring the return value
   (as `e_free(buf);` already does) still compiles and behaves the same. */
#define initRuntimeMemory eccles_rtmem_init
#define e_malloc           eccles_rt_malloc
#define e_free             eccles_rt_free
#define e_getStats         eccles_rt_get_stats
#define e_PoolStats        eccles_rt_stats_t
#endif

/* search and return an available buffer from the pool. Returns NULL for a
   zero-byte request, if init hasn't run/succeeded, or if no pool has a
   contiguous run of free blocks large enough. Every pointer returned is
   aligned to ECCLES_RT_ALIGNMENT bytes. Caller MUST call eccles_rt_free()
   once done with the buffer. */
uint8_t* eccles_rt_malloc(size_t size);

/* give this buffer back to the allocator. Safe to call with NULL (returns
   ECCLES_RT_FREE_NULL, not an error). Check the return value if you want
   to know *why* an invalid free was rejected instead of just that it was. */
eccles_rt_free_status_t eccles_rt_free(uint8_t* buffer);

/* snapshot of how many blocks are used/free in each pool right now */
eccles_rt_stats_t eccles_rt_get_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* ECCLES_RTMEM_H */
