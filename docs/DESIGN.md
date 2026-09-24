# Design & reference

Deep-dive reference for EcclesRTLib: how the allocator works, every configuration knob, the locking backends, and the test suite. For a quick tour see the [README](../README.md).

**Contents:** [How it works](#how-it-works) · [Configuring](#configuring) · [Locking](#locking-threadisr-safety) · [Free status codes](#what-eccles_rt_free-tells-you) · [Debug logging](#debug-logging) · [API](#api) · [Testing](#testing)

## How it works

One memory budget (`ECCLES_RT_MEM_SIZE` bytes) is split into three pools of
fixed block sizes — small (A), medium (B), large (C). `eccles_rt_malloc()`
hands out the smallest block that fits; requests bigger than one large block
get a contiguous run of blocks, tried from the largest pool down to the
smallest so fragmentation in one pool doesn't fail a request the others
could still satisfy — but each individual allocation only ever comes from
*one* pool; blocks from two different pools are never combined into a single
allocation. `eccles_rt_free()` validates the pointer (real block start? not
already free? not the middle of a run?) before touching any bookkeeping, and
tells you *why* it rejected an invalid one via its return value.

## Configuring

Copy `eccles_rtmem_config.h` next to your sources and uncomment whatever you
want to change — every setting in it is documented inline and commented out
by default, so an unmodified copy changes nothing. This file is picked up
automatically (via `__has_include`, supported by GCC/Clang/avr-gcc/IAR/Arm
Compiler 6) by *every* file that includes `eccles_rtmem.h`, including
`eccles_rtmem.c` itself — which matters, because pool sizes are baked in at
preprocess time, so your app code and the library's own `.c` file both need
to see *identical* macros or they'll disagree about how big the pools are.
The alternative is compiler command-line defines (`-DECCLES_RT_MEM_SIZE=4096`
via PlatformIO's `build_flags`, CMake's `target_compile_definitions`, etc.),
which apply to every source file automatically too. What does **not** work:
`#define`s directly above your own `#include "eccles_rtmem.h"` — the library's
own `.c` file won't see those.

### Memory budget

Defaults, if you don't configure anything:

| | RAM-constrained MCUs | everything else |
|---|---|---|
| detected via | `__AVR__`, `__MSP430__`, `__STM8__`, 8-bit PIC (`__XC8__`/`_PIC12`/`_PIC16`/`_PIC18`), 8051 (`__C51__`/`__CX51__`/SDCC-mcs51), entry-level Cortex-M0 STM32 (`STM32F0`/`G0`/`L0`/`C0`) | ESP32/ESP8266, bigger STM32 lines, SAMD, RP2040, nRF52, PIC32, desktop test builds, anything not in the left column |
| total budget | 1 KB | 20 KB |
| block sizes (A/B/C) | 16 / 32 / 64 bytes | 64 / 128 / 256 bytes |
| split | 25% / 25% / 50% | 25% / 25% / 50% |

This is a **family-level guess, not a per-chip one** — a given family can
span a huge RAM range (an STM32F1 might have 4 KB or 128 KB depending on the
exact part). Treat the auto-default as a convenience for prototyping only;
set `ECCLES_RT_MEM_SIZE` explicitly for anything you intend to ship. On
GCC/Clang, leaving it on auto-detect triggers a `#warning` reminding you of
this every time you build.

### Block sizes, split, and validation

Block sizes must be powers of two (so the allocator can use bit shifts
instead of multiply/divide on the hot path — real savings on an AVR without
a hardware multiplier) and must satisfy `A <= B <= C`, since
`eccles_rt_malloc()`'s size routing assumes that ordering. `ECCLES_RT_POOL_
A_PERCENT + ECCLES_RT_POOL_B_PERCENT` can't exceed 100. All three of these
are checked at **compile time** with a clear `#error` if violated, rather
than silently misbehaving or overflowing at runtime.

If you set a non-power-of-two block size anyway (which is otherwise
allowed), the library still works — it detects this at `eccles_rtmem_init()`
and falls back to plain division for that pool — just slower.

Each pool's block *count* also doubles as a "run length" value in a 1-byte
registry entry (255 is reserved to mean "middle of a multi-block run"), so
the combined block count across A+B+C can't exceed 255 — also a compile-time
`#error` if your settings would exceed it.

### Alignment

Every pointer `eccles_rt_malloc()` returns is aligned to `ECCLES_RT_ALIGNMENT`
bytes (default 8), so it's safe to cast to a wider type. Automatic in heap
mode (`malloc()` already guarantees max alignment); applied via a GCC/Clang
`__attribute__((aligned(...)))` in static mode. On a non-GCC/Clang toolchain
in static mode, align the pool array yourself if you need this guarantee.

## Locking (thread/ISR safety)

`eccles_rt_malloc`/`_free`/`_get_stats` share one lock. In priority order:

0. **`ECCLES_RT_MEM_NO_LOCK`** — strips locking out entirely: no mutex
   object, no interrupt masking, nothing. Wins over everything below,
   including a manual override. Only safe if these calls are always made
   from one thread/task/ISR context, or you're doing your own coarser
   locking around whole call sequences.
1. Your own **manual override** — define all five of `ECCLES_RT_LOCK_INIT()`,
   `ECCLES_RT_LOCK(m, st)`, `ECCLES_RT_UNLOCK(m, st)`, `ECCLES_RT_MUTEX_T`,
   and `ECCLES_RT_LOCK_STATE_T` to plug in any RTOS's mutex. `st` is a
   variable declared fresh at each call site (not a shared static) — this
   is what makes interrupt-mask-style locking actually reentrant.
2. **Zephyr** — `k_mutex`, if `__ZEPHYR__` looks present.
3. **FreeRTOS** — a real semaphore, if FreeRTOS looks present (ESP-IDF,
   ESP32 Arduino core, or `#define ECCLES_RT_USE_FREERTOS` yourself).
4. **CMSIS-RTOS2/RTX5** — `osMutex`, if `cmsis_os2.h` is reachable (typical
   of STM32CubeMX-generated FreeRTOS/RTX5 projects).
5. **Pico SDK** — `critical_section_t`, on RP2040/RP2350 (needed, not
   optional: RP2040 is dual-core, and masking interrupts only blocks the
   core that called it — `critical_section_t` is spinlock-backed and safe
   across both).
6. **AVR** — `cli()`/`sei()`, saving/restoring `SREG` into a call-site-local
   variable, if `__AVR__` is defined.
7. **MSP430** — `__disable_interrupt()`/`__enable_interrupt()`, same
   save/restore pattern, if `__MSP430__` is defined.
8. **Microchip XC16/XC32** — `__builtin_disable_interrupts()`, same pattern,
   on dsPIC/PIC24/PIC32.
9. **generic Cortex-M** — PRIMASK save/disable/restore (IAR, Keil/ARMCC, or
   GCC/Clang, whichever your toolchain is), for any ARM target not already
   matched above.
10. **no-op**, otherwise — assumes a single-threaded, non-preemptive
    bare-metal build. This is where STM8, 8-bit PIC, and 8051 land, since
    their interrupt-disable syntax differs too much across
    Cosmic/IAR/SDCC/Keil-C51 to guess portably. Define the manual override
    from step 1 yourself if you need real locking on one of these.

**Reentrancy/ISR notes:**
- These functions are not designed to call themselves reentrantly (they
  never do this internally, and you shouldn't either).
- Interrupt-masking backends (AVR/MSP430/XC16/XC32/generic Cortex-M) are
  ISR-safe for both allocation and free — an ISR literally can't preempt
  while the lock is held, since interrupts are masked for the whole section.
- RTOS-mutex backends (FreeRTOS/Zephyr/CMSIS-RTOS2) are **not** ISR-safe by
  default — don't call these functions from an ISR when one of these is
  active unless you've swapped in that RTOS's FromISR-style API yourself.

## What `eccles_rt_free()` tells you

Unlike a typical `free()`, this one returns an `eccles_rt_free_status_t` so
you can find out *why* an invalid free was rejected, not just that it was:

```c
typedef enum {
    ECCLES_RT_FREE_OK = 0,
    ECCLES_RT_FREE_NULL,          /* buffer was NULL - harmless no-op */
    ECCLES_RT_FREE_NOT_READY,     /* eccles_rtmem_init() hasn't run/succeeded yet */
    ECCLES_RT_FREE_OUT_OF_RANGE,  /* pointer isn't inside any pool this allocator owns */
    ECCLES_RT_FREE_MISALIGNED,    /* inside a pool, but not on a block boundary */
    ECCLES_RT_FREE_DOUBLE_FREE,   /* that block is already marked free */
    ECCLES_RT_FREE_MID_RUN        /* pointer into the middle of a multi-block allocation */
} eccles_rt_free_status_t;
```

Existing code that just calls `eccles_rt_free(buf);` and ignores the return
value keeps working exactly as before.

## Debug logging

```c
#define ECCLES_RT_DEBUG          /* turns logging on */
/* optional: point it at your own output instead of the printf() default */
#define ECCLES_RT_LOG_LINE(msg) Serial.println(msg)
#include "eccles_rtmem.h"
```

Logs allocation failures and rejected frees — never anything on the success
path.

## API

```c
bool                      eccles_rtmem_init(void);  /* call once; safe to call again (no-op) */
uint8_t*                  eccles_rt_malloc(size_t size);
eccles_rt_free_status_t   eccles_rt_free(uint8_t *buffer);
eccles_rt_stats_t         eccles_rt_get_stats(void); /* used/free block counts per pool */
```

`eccles_rtmem_init()` is **idempotent**: a second call is a safe no-op
rather than re-`malloc()`ing a fresh pool (heap mode) or otherwise disturbing
buffers you've already handed out. It returns `false` only if
`ECCLES_RT_MEM_USE_HEAP` is set and the underlying `malloc()` failed; you can
call it again later to retry.

## Testing

`tests/` has:
- **`test_unit.c`** — deterministic checks: class-boundary routing,
  multi-block allocation, mid-run/double/out-of-range/misaligned free
  rejection, pool exhaustion and reuse, a fragmentation scenario proven to
  fail correctly even with no fallback room, and idempotent re-init.
- **`test_stress.c`** — randomized alloc/free with canary-byte corruption
  detection (every live buffer is stamped with a unique byte pattern and
  re-checked before every free) and `eccles_rt_get_stats()` invariant
  checks (`used + free == count`) throughout a run. Run with
  `./test_stress [op_count] [seed]`; defaults to 500,000 ops. Passes clean
  under both AddressSanitizer and UndefinedBehaviorSanitizer.
- **`test_smoke.c`** — a minimal, config-agnostic check safe for any valid
  configuration, including a 1-block-per-pool minimum.
- **`run_tests.sh`** — runs the above across 8 distinct configurations
  (default, heap mode, `NO_LOCK`, a 1 KB weak-MCU-scale budget, a custom
  A/B/C split, custom block sizes, a near-the-255-block-cap configuration,
  and a minimum-viable pool).

```
make test        # or: cd tests && ./run_tests.sh
make sanitize    # same matrix under ASan + UBSan
```

This suite is how a real out-of-bounds write was caught during review — see the [CHANGELOG](../CHANGELOG.md).
