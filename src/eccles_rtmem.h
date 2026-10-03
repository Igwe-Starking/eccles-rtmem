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
      - eccles_rt_malloc() picks the smallest class that fits the request
        as its first try, then escalates to progressively larger classes
        if that one has no free block right now (never steps down to a
        smaller class - its blocks might not be big enough). For requests
        bigger than one C block, it needs a contiguous multi-block run
        instead, tried largest-class-first by default (see
        ECCLES_RT_MALLOC_MIN_WASTE below for an alternative). Either way,
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
    pools, a request can still fail even when the *total* free memory
    across all three pools would have been enough - a single allocation
    is always satisfied from one class only, and only the classes big
    enough to hold the request are ever tried.

    ---------------------------------------------------------------------
    CONFIGURATION (define these BEFORE including this header, if you want
    something other than the defaults)
    ---------------------------------------------------------------------

    ECCLES_RT_MEM_SIZE
        Total budget, in bytes, for the three pools combined. If you don't
        set this, it's derived (in priority order) from: your declared
        ECCLES_RT_TOTAL_RAM_SIZE if you set one, else a lookup table of
        common chips' known RAM sizes if your chip is in it, else a
        coarse per-family guess (1 KB on families that are *usually*
        RAM-constrained, 20 KB otherwise) as a last resort. See
        ECCLES_RT_TOTAL_RAM_SIZE below and the platform table further down.

        IMPORTANT: the *last-resort* fallback is a family-level guess, not
        a per-chip one. An MCU family can span a huge RAM range (a "big"
        STM32F1 part might have 4 KB or 128 KB depending on the exact
        model) - that guess can be outright wrong, even dangerously so,
        for your specific chip. Treat it as a convenience for
        prototyping/examples only; for anything you intend to ship, either
        set ECCLES_RT_TOTAL_RAM_SIZE (so at least the *proportion* is
        sensible) or set ECCLES_RT_MEM_SIZE directly to a number you've
        checked against your part's actual RAM.

    ECCLES_RT_TOTAL_RAM_SIZE
        Your chip's actual total RAM, in bytes, if you know it (check the
        datasheet - most embedded developers do know this, even though
        this header structurally can't: a macro like __AVR__ says "this
        is some AVR," not "this chip has 2048 bytes"). When set, the
        budget becomes a PERCENTAGE of this number instead of a flat 1 KB
        or 20 KB, tiered so smaller chips don't hand over an outsized
        fraction of their tiny RAM, while bigger chips don't get a
        needlessly tiny sliver of their large RAM:

            RAM < 4 KB    -> 50% of RAM
            RAM < 10 KB   -> 25% of RAM
            RAM < 100 KB  -> 10% of RAM
            RAM >= 100 KB -> 5% of RAM

        Known rough edge: there's a small NON-MONOTONIC step exactly at
        each boundary above - a chip with 4095 bytes of RAM computes
        ~2048 bytes of budget (50%), but one with 4096 bytes computes only
        ~1024 bytes (25%): the budget can go DOWN as RAM goes up, right at
        the line. If your part's RAM sits close to one of these
        boundaries, skip this and set ECCLES_RT_MEM_SIZE directly instead.

        This doesn't need to be set for a modest list of common chips
        (mostly classic 8-bit Arduino/LaunchPad parts) - see the lookup
        table in the ECCLES_RT_MEM_SIZE detection code below - since their
        compiler already tells us their exact RAM size via a
        chip-specific macro (e.g. __AVR_ATmega328P__) with no cooperation
        needed from your build system. That table is a curated
        convenience, not exhaustive; if your chip isn't in it, this is the
        macro to set.

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
        Block size, in bytes, of each pool. Must be a multiple of
        ECCLES_RT_ALIGNMENT, and satisfy A <= B <= C (both checked at
        compile time). Default: 16 / 32 / 64 if ECCLES_RT_MEM_SIZE <= 4096,
        else 64 / 128 / 256.

        A power of two is RECOMMENDED but not required: it lets the
        allocator use a bit shift instead of a division on the hot path
        (real savings on an AVR without a hardware multiplier). A
        non-power-of-two size still works correctly - detected at
        eccles_rtmem_init() and handled with plain division for that pool
        instead - just somewhat slower. There's no compile-time check
        enforcing power-of-two specifically, only the alignment-multiple
        and ordering checks above.

    ECCLES_RT_POOL_A_PERCENT / _B_PERCENT
        Percentage of ECCLES_RT_MEM_SIZE handed to pool A and pool B; pool C
        gets whatever remains. Default: 25 / 25 (pool C gets ~50%), matching
        the ratio the original fixed-size version shipped with. Their sum
        can't exceed 100 (checked at compile time).

    ECCLES_RT_MALLOC_MIN_WASTE
        Changes how a request bigger than one C block (which needs a
        contiguous multi-block run) picks which class to take that run
        from. Default (undefined): try the largest class (C) first, then
        B, then A - this tends to preserve the small classes' block-count
        capacity for their intended job (frequent small allocations)
        rather than spending it on one large request, at the cost of
        possibly wasting more bytes on that one request than a smaller-
        class choice would have. Define this to instead try whichever
        class wastes the FEWEST bytes on this specific request first,
        falling back to the next-least-wasteful feasible class if that
        one's too fragmented to satisfy it right now.

        Worked example with the default 64/128/256-byte classes: a
        513-byte request needs 3 C-blocks (768 bytes, 255 wasted) by
        default. With ECCLES_RT_MALLOC_MIN_WASTE, it'd try 9 A-blocks
        first instead (576 bytes, only 63 wasted) - less waste on THIS
        call, but 9 of pool A's blocks are now unavailable to whatever
        else was relying on pool A for small, frequent allocations. There
        is no universally-correct choice here: minimizing bytes wasted
        per call and preserving small-pool capacity for future
        allocations are genuinely in tension, and which one matters more
        depends on your workload (mostly-large-transfer firmware vs.
        mixed small+occasional-large firmware). Computing the waste for
        all 3 classes is O(1) either way (a handful of divisions, done
        once per oversized-allocation call, not in a hot loop) - the
        default not using it is a deliberate choice about which failure
        mode to bias against, not a performance concern. On an exact tie
        (a request size that's a common multiple of more than one block
        size, so two classes waste the same number of bytes), the larger
        class wins the tie - the same "leave the small pools alone" bias
        the default ordering uses, applied only as a tiebreaker here.

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

        There is a SIXTH piece worth knowing about even though only five
        are required: ECCLES_RT_MUTEX_VALID(m), used internally to check
        whether ECCLES_RT_LOCK_INIT() actually produced a usable mutex.
        It defaults to `(m) != NULL`, which assumes ECCLES_RT_MUTEX_T is
        pointer-like (true for every built-in backend above). If your
        custom ECCLES_RT_MUTEX_T is a plain integer/enum handle, a
        struct-by-value, or anything else where "compare to NULL" isn't
        meaningful (or where 0 is a perfectly valid handle, not an
        "invalid" sentinel), define ECCLES_RT_MUTEX_VALID(m) yourself too,
        or eccles_rtmem_init() may report success even when your lock
        creation actually failed - or, just as bad, report failure on a
        perfectly good handle whose valid value happens to be 0.

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
          8) Cortex-M           - PRIMASK save/disable/restore in a call-
                                  site-local variable, using whichever
                                  intrinsic/asm your toolchain wants (IAR /
                                  Keil-ARMCC / GCC-Clang), for Cortex-M
                                  targets specifically, not "any ARM" -
                                  PRIMASK is an M-profile-only register and
                                  doesn't exist on Cortex-A/R or older
                                  ARM7TDMI-class cores, which fall through
                                  to the no-op case below instead of
                                  risking an invalid instruction (covers
                                  most STM32 lines, SAMD, nRF52 without an
                                  RTOS, etc.)
          9) no-op, otherwise  - assumes a single-threaded, non-preemptive
                                  bare-metal build. If that's wrong for your
                                  target (some other RTOS, an 8-bit chip
                                  without its own case above, a non-Cortex-M
                                  ARM core, ...), either
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
    cursor looking for a free run. For a single-block request that's
    O(pool's block count) in the worst case (bounded by your configured
    budget, not unbounded). For a MULTI-block request (a size bigger than
    one C block), each of up to that pool's block count starting positions
    can itself require scanning up to the requested run length to check
    it's actually free, making the true worst case O(block count * run
    length) - quadratic in a badly fragmented pool, not linear. This is
    "bounded" and "predictable for a fixed configuration", not a hard
    real-time constant-time guarantee: how long a given call takes still
    depends on how fragmented that pool is at that moment.

    This matters more than usual for the interrupt-masking locking
    backends (AVR/MSP430/XC16/XC32/Cortex-M) specifically, since the ENTIRE
    scan - worst case included - runs with interrupts disabled: a
    fragmented pool doesn't just mean a slower allocation, it means
    LONGER-MASKED INTERRUPTS, which can matter for latency-sensitive ISRs
    elsewhere in your firmware. No worst-case timing has been measured on
    real hardware for this release - treat this as bounded-but-not-
    formally-real-time until you've benchmarked it on your own target, and
    keep it in mind particularly if you rely on tight interrupt latency
    elsewhere in the same firmware.
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
  /* NOTE: no #warning here on purpose - it used to remind you to set this
     explicitly, but #warning is classified as a warning diagnostic by
     GCC/Clang, so a project building with -Werror (ESP-IDF's and
     PlatformIO's stricter presets, and plenty of CI configs, do this by
     default) turned that friendly reminder into a hard build failure.
     The reminder still lives in this header's comments (see the
     ECCLES_RT_MEM_SIZE note above) instead of as a compiler diagnostic. */

  /* Step 1: do we know (or can we look up) your chip's actual total RAM,
     in bytes? If so, the budget is computed as a PERCENTAGE of it instead
     of a flat number - a flat number is either far too big a slice of a
     tiny chip's RAM, or needlessly tiny relative to a huge chip's RAM.
     Smaller RAM gets a bigger percentage (there's less of it competing
     for the slice) and bigger RAM gets a smaller one:

         RAM < 4 KB    -> 50%
         RAM < 10 KB   -> 25%
         RAM < 100 KB  -> 10%
         RAM >= 100 KB -> 5%

     Known rough edge: there's a small NON-MONOTONIC step exactly at each
     boundary - e.g. 4095 bytes of RAM computes ~2048 bytes of budget
     (50%), but 4096 bytes computes only ~1024 bytes (25%): budget can go
     DOWN as RAM goes up, right at the line. If your part's RAM sits close
     to one of these boundaries, just set ECCLES_RT_MEM_SIZE directly
     instead of relying on this tiering.

     Where the RAM figure comes from, in priority order: */
  #if defined(ECCLES_RT_TOTAL_RAM_SIZE)
    /* (a) you told us directly - see the ECCLES_RT_TOTAL_RAM_SIZE note
       below. Always trust this over any guess. */
    #define ECCLES_RT__RAM_SIZE (ECCLES_RT_TOTAL_RAM_SIZE)
  #elif defined(__AVR_ATtiny25__)
    #define ECCLES_RT__RAM_SIZE 128ul
  #elif defined(__AVR_ATtiny45__)
    #define ECCLES_RT__RAM_SIZE 256ul
  #elif defined(__AVR_ATtiny85__)
    #define ECCLES_RT__RAM_SIZE 512ul
  #elif defined(__AVR_ATmega168__) || defined(__AVR_ATmega168A__) || defined(__AVR_ATmega168P__) || defined(__AVR_ATmega168PA__)
    #define ECCLES_RT__RAM_SIZE 1024ul
  #elif defined(__AVR_ATmega328__) || defined(__AVR_ATmega328P__) || defined(__AVR_ATmega328PB__)
    #define ECCLES_RT__RAM_SIZE 2048ul       /* Uno, Nano, Pro Mini */
  #elif defined(__AVR_ATmega32U4__)
    #define ECCLES_RT__RAM_SIZE 2560ul       /* Leonardo, Micro */
  #elif defined(__AVR_ATmega644__) || defined(__AVR_ATmega644A__) || defined(__AVR_ATmega644P__) || defined(__AVR_ATmega644PA__)
    #define ECCLES_RT__RAM_SIZE 4096ul
  #elif defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
    #define ECCLES_RT__RAM_SIZE 8192ul       /* Mega, Mega2560 */
  #elif defined(__AVR_ATmega1284__) || defined(__AVR_ATmega1284P__)
    #define ECCLES_RT__RAM_SIZE 16384ul
  #elif defined(__MSP430G2452__)
    #define ECCLES_RT__RAM_SIZE 256ul
  #elif defined(__MSP430G2553__)
    #define ECCLES_RT__RAM_SIZE 512ul        /* common LaunchPad chip */
  #elif defined(__MSP430F5529__)
    #define ECCLES_RT__RAM_SIZE 8192ul       /* common LaunchPad chip */
  #endif
  /* (b) otherwise, ECCLES_RT__RAM_SIZE stays undefined and step 2 below
     falls back to the coarse per-family guess this header always had.
     This table is a curated convenience list of common chips (mostly
     classic 8-bit Arduino/LaunchPad parts, whose compiler automatically
     defines a chip-specific macro from -mmcu with no build-system
     cooperation needed, unlike many vendor macros e.g. STM32's, which
     usually depend on a separate -D flag your build system has to also
     remember to pass) - it is NOT exhaustive. If your chip isn't in it,
     either add your own #elif here (in a copy of this header, or your
     eccles_rtmem_config.h) or just define ECCLES_RT_TOTAL_RAM_SIZE. */

  #if defined(ECCLES_RT__RAM_SIZE)
    #if (ECCLES_RT__RAM_SIZE) < 4096ul
      #define ECCLES_RT_MEM_SIZE ((ECCLES_RT__RAM_SIZE) / 2ul)   /* 50% */
    #elif (ECCLES_RT__RAM_SIZE) < 10240ul
      #define ECCLES_RT_MEM_SIZE ((ECCLES_RT__RAM_SIZE) / 4ul)   /* 25% */
    #elif (ECCLES_RT__RAM_SIZE) < 102400ul
      #define ECCLES_RT_MEM_SIZE ((ECCLES_RT__RAM_SIZE) / 10ul)  /* 10% */
    #else
      #define ECCLES_RT_MEM_SIZE ((ECCLES_RT__RAM_SIZE) / 20ul)  /* 5% */
    #endif
  #else
    /* Step 2 (last resort): no RAM figure known or declared - the
       original coarse per-family guess: 1 KB on families that are
       *usually* RAM-constrained, 20 KB otherwise. This is a family-level
       guess, not a per-chip one - see the IMPORTANT note above. */
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
#endif

#if (ECCLES_RT_MEM_SIZE) == 0
  #error "eccles_rtmem: ECCLES_RT_MEM_SIZE must be nonzero."
#endif

/* ===================================================================== *
 *  block sizes (power of two recommended but not required; must be a
 *  multiple of ECCLES_RT_ALIGNMENT, and A <= B <= C)
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

/* each pool is clamped to a MINIMUM of 1 block (see ECCLES_RT__CLAMP above),
   so a budget too small to give every pool even one real block silently
   reserves MORE bytes than ECCLES_RT_MEM_SIZE asked for, instead of
   failing or shrinking further - e.g. ECCLES_RT_MEM_SIZE=1 with default
   16/32/64-byte classes reserves 16+32+64=112 bytes, 112x the requested
   budget. Catch that at compile time instead of silently over-allocating.
   (no (size_t) casts here - #if is preprocessor arithmetic and doesn't
   understand C types; that's fine, the preprocessor's own arithmetic is
   done in a type at least as wide as intmax_t/uintmax_t regardless) */
#define ECCLES_RT__ACTUAL_POOL_BYTES \
    (ECCLES_RT_POOL_A_COUNT * ECCLES_RT_BLOCK_A_SIZE + \
     ECCLES_RT_POOL_B_COUNT * ECCLES_RT_BLOCK_B_SIZE + \
     ECCLES_RT_POOL_C_COUNT * ECCLES_RT_BLOCK_C_SIZE)

#if ECCLES_RT__ACTUAL_POOL_BYTES > ECCLES_RT_MEM_SIZE
  #error "eccles_rtmem: ECCLES_RT_MEM_SIZE is too small to give every pool class " \
         "at least one real block without exceeding the budget you asked for. " \
         "Raise ECCLES_RT_MEM_SIZE, lower a block size, or reconsider the " \
         "A/B_PERCENT split for this budget."
#endif

#ifndef ECCLES_RT_ALIGNMENT
  #define ECCLES_RT_ALIGNMENT 8
#endif

#if (ECCLES_RT_ALIGNMENT) == 0 || ((ECCLES_RT_ALIGNMENT) & ((ECCLES_RT_ALIGNMENT) - 1)) != 0
  #error "eccles_rtmem: ECCLES_RT_ALIGNMENT must be a nonzero power of two."
#endif

/* every block in a class must be a multiple of ECCLES_RT_ALIGNMENT, or
   only the FIRST block in that class (and the whole pool array's start)
   would actually land on an aligned address - every later block's offset
   is (block index * block size) bytes into the pool, so if block size
   itself isn't a multiple of the alignment, later blocks silently drift
   off it. Checked here instead of silently handing out misaligned
   pointers from block 2 onward. */
#if (ECCLES_RT_BLOCK_A_SIZE) % (ECCLES_RT_ALIGNMENT) != 0 \
 || (ECCLES_RT_BLOCK_B_SIZE) % (ECCLES_RT_ALIGNMENT) != 0 \
 || (ECCLES_RT_BLOCK_C_SIZE) % (ECCLES_RT_ALIGNMENT) != 0
  #error "eccles_rtmem: ECCLES_RT_BLOCK_A/B/C_SIZE must each be a multiple of " \
         "ECCLES_RT_ALIGNMENT, or blocks after the first in a class won't " \
         "actually be aligned. Either raise the block size(s) to a multiple " \
         "of ECCLES_RT_ALIGNMENT, or lower ECCLES_RT_ALIGNMENT to fit."
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
    ECCLES_RT_FREE_ALREADY_FREE,  /* that block is already marked free - not necessarily
                                      a literal double-free of the same allocation: a
                                      stale pointer that happens to collide with some
                                      OTHER currently-free block looks identical from
                                      here, since nothing records which specific
                                      allocation last owned a slot */
    ECCLES_RT_FREE_MID_RUN        /* pointer into the middle of a multi-block allocation, not its start */
} eccles_rt_status_t;

/* eccles_rtmem_init() must be called once (e.g. from setup()/app_main())
   before any eccles_rt_malloc/_free/_get_stats call. Calling it again
   after it has already succeeded is a safe no-op that just returns true,
   so you can't accidentally re-malloc() a fresh pool out from under
   buffers you've already handed out (heap mode) or otherwise disturb
   state a second call might have reset. Returns false only if
   ECCLES_RT_MEM_USE_HEAP is defined and the underlying malloc() failed -
   in that case eccles_rt_malloc/_free/_get_stats stay inert (NULL/no-op/
   zeroed) rather than touching unallocated memory, and you can call
   eccles_rtmem_init() again later to retry.

   PRECONDITION - not covered by "idempotent" above: eccles_rtmem_init()
   itself is NOT safe to call from two contexts at the same time (there's
   no lock protecting the init sequence, since the lock doesn't exist yet
   until init creates it - a chicken-and-egg problem this library doesn't
   attempt to solve generically across every one of its many locking
   backends). Call it once, from a single thread/task, BEFORE starting any
   other thread/task/ISR that might also call it or call eccles_rt_malloc/
   _free - the same precondition most embedded libraries' init functions
   have (e.g. FreeRTOS itself doesn't make xTaskCreate() safe to race
   against the scheduler starting). "Idempotent" describes repeat calls
   from that same single initializing context (e.g. a library that also
   depends on this one calling it again defensively), not concurrent ones. */
bool eccles_rtmem_init(void);

#ifndef ECCLES_RT_NO_DEPRECATED_ALIASES
/* deprecated aliases for the pre-review names - define
   ECCLES_RT_NO_DEPRECATED_ALIASES before including this header if you
   don't want them. e_free's old callers see it return a
   eccles_rt_status_t now instead of void; ignoring the return value
   (as `e_free(buf);` already does) still compiles and behaves the same. */
#define initRuntimeMemory eccles_rtmem_init
#define e_malloc           eccles_rt_malloc
#define e_free             eccles_rt_free
#define e_getStats         eccles_rt_get_stats
#define e_PoolStats        eccles_rt_stats_t
/* eccles_rt_free_status_t was this library's name for eccles_rt_status_t
   for one prior revision - not part of the original pre-review API, but
   aliased anyway for consistency with everything else in this block */
typedef eccles_rt_status_t eccles_rt_free_status_t;
/* ECCLES_RT_FREE_DOUBLE_FREE was renamed to ECCLES_RT_FREE_ALREADY_FREE
   for precision (see the enum definition above for why) */
#define ECCLES_RT_FREE_DOUBLE_FREE ECCLES_RT_FREE_ALREADY_FREE
#endif

/* search and return an available buffer from the pool. Returns NULL for a
   zero-byte request, if init hasn't run/succeeded, or if no pool has a
   contiguous run of free blocks large enough. Every pointer returned is
   aligned to ECCLES_RT_ALIGNMENT bytes. Caller MUST call eccles_rt_free()
   once done with the buffer.

   NOT SAFE TO CALL FROM AN ISR if the active locking backend is an RTOS
   mutex (FreeRTOS/Zephyr/CMSIS-RTOS2 - see the locking cascade above) -
   those backends' blocking lock/unlock calls aren't meant to run in
   interrupt context. It IS safe from an ISR with any of the interrupt-
   masking backends (AVR/MSP430/XC16/XC32/generic Cortex-M/NO_LOCK),
   since those can't be preempted while the lock is "held" in the first
   place. If you don't know which backend is active for your build, treat
   this as ISR-unsafe. Also not safe to call reentrantly (from within
   itself) under any backend - see the reentrancy notes above. */
uint8_t* eccles_rt_malloc(size_t size);

/* give this buffer back to the allocator. Safe to call with NULL (returns
   ECCLES_RT_FREE_NULL, not an error). Check the return value if you want
   to know *why* an invalid free was rejected instead of just that it was.

   Same ISR-safety caveat as eccles_rt_malloc() above applies here too.

   This can't detect every use-after-free: it validates that a pointer
   lands on a real, currently-allocated block boundary, but if that exact
   address has since been reused by a DIFFERENT live allocation (freed
   once already, then handed back out by a later eccles_rt_malloc call),
   this has no way to tell "the same logical allocation, freed twice" apart
   from "a stale pointer that happens to collide with someone else's
   current allocation" - it will look like an ordinary, valid free of
   whatever's live there now, silently corrupting that unrelated
   allocation's use of it. This isn't unique to this allocator (the same
   is true of most fixed-pool/slab allocators without extra per-block
   generation-counter bookkeeping, which this one doesn't keep) - it's
   just worth being explicit about. */
eccles_rt_status_t eccles_rt_free(uint8_t* buffer);

/* calloc() equivalent: eccles_rt_malloc(count * size), zero-initialized,
   with an overflow-safe multiply (returns NULL instead of wrapping around
   to a too-small allocation if count * size would overflow size_t) -
   exactly the property calloc() has over malloc()+memset() by hand. Costs
   the same as malloc() for the reservation, plus a memset() over the
   allocated block(s) (which can be bigger than count*size, rounded up to
   block granularity). */
uint8_t* eccles_rt_calloc(size_t count, size_t size);

/* realloc() equivalent, with the same core contract: on success, the
   first min(old allocated capacity, new_size) bytes of content are
   preserved, and the old pointer must be treated as freed - use the
   RETURNED pointer from here on, not the one you passed in, since it may
   have moved. On FAILURE (returns NULL for a nonzero new_size), the
   original pointer is untouched and still valid to use/free yourself -
   exactly like the standard library's realloc(), and worth calling out
   because "forgetting realloc failure doesn't free the old pointer" is a
   classically common bug in code that assumes otherwise.

   realloc(NULL, size) behaves like eccles_rt_malloc(size).
   realloc(ptr, 0) frees ptr and returns NULL (the C committee has gone
   back and forth on this exact case across standard revisions - this
   picks the long-standing "acts like free()" behavior most real-world
   code already assumes, rather than the alternative of returning a valid
   zero-size allocation).

   Implementation note: resizing happens IN PLACE whenever it can, inside
   one critical section. The run's block count is recomputed for new_size
   in the SAME pool:
     - fewer blocks: the trailing excess blocks are released back to the
       pool immediately (bytes are not zeroed, same as free) and the
       pointer is returned unchanged;
     - same block count: pointer returned unchanged, nothing touched;
     - more blocks: the boundary right after the run is checked, and if
       the extra blocks are free and still inside the same pool they are
       absorbed and the pointer is returned unchanged - no copy;
     - otherwise (neighbour in use, pool boundary, or the request is
       bigger than the whole pool): allocate-new + copy + free-old, which
       may land in a different class. On failure the original is intact.
   A shrunk allocation stays in its original class (a 5-block C run
   shrunk to 10 bytes becomes one C block, not an A block) - that is the
   price of never copying on a shrink. */
uint8_t* eccles_rt_realloc(uint8_t *ptr, size_t new_size);

/* LIMITED aligned_alloc() equivalent - NOT a general one. Every pointer
   eccles_rt_malloc() returns is already aligned to ECCLES_RT_ALIGNMENT
   bytes unconditionally (see ECCLES_RT_ALIGNMENT above), so this succeeds
   at zero extra cost - it's just eccles_rt_malloc(size) - whenever
   `alignment` is already satisfied by that existing guarantee (alignment
   <= ECCLES_RT_ALIGNMENT and ECCLES_RT_ALIGNMENT is a multiple of it,
   which since both are required to be powers of two just means alignment
   <= ECCLES_RT_ALIGNMENT). Returns NULL for any alignment bigger than
   that, or that isn't itself a power of two - supporting arbitrary
   alignments bigger than the built-in one would need per-allocation
   offset bookkeeping (to remember how far into a block the returned
   pointer was shifted, so eccles_rt_free() could find the real block
   start again), which is real added complexity/RAM cost this library
   deliberately doesn't take on. If you need a bigger guaranteed
   alignment for everything, raise ECCLES_RT_ALIGNMENT itself instead. */
uint8_t* eccles_rt_aligned_alloc(size_t alignment, size_t size);

/* wipes every pool back to fully-free: every block's registry entry is
   cleared, every pool's cursor resets to the start of its own slice, and
   the used-block counters eccles_rt_get_stats() reads all go back to 0.
   The allocator is immediately ready to use again afterward - no need to
   call eccles_rtmem_init() again, no reallocation happens in heap mode
   (the same pool/registry memory is reused, just logically cleared).

   IMPORTANT: this does NOT zero the actual pool bytes, only the
   bookkeeping - same "don't pay for zeroing nobody asked for" philosophy
   as eccles_rt_free() (see its note on this). It also does NOT know or
   care that you might still be holding pointers from before the reset -
   every one of them is invalidated as far as the allocator's concerned,
   and using or freeing one afterward is exactly as unsafe as using a
   pointer after eccles_rt_free() already returned it, possibly worse
   (the address might now belong to a completely different live
   allocation). This is meant for "start over with a clean slate and
   nothing else is still relying on prior allocations" situations - e.g.
   between test cases, or a deliberate soft-reset of a subsystem - not as
   a general-purpose "free everything I forgot to free" safety net.

   Returns false (and does nothing) if eccles_rtmem_init() hasn't
   succeeded yet - there's nothing to reset in that case. */
bool eccles_rt_reset(void);

/* snapshot of how many blocks are used/free in each pool right now */
eccles_rt_stats_t eccles_rt_get_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* ECCLES_RTMEM_H */
