# Design & reference

Deep-dive reference for EcclesRTLib: how the allocator works, every configuration knob, the locking backends, the full API, and the test suite. For a quick tour see the [README](../README.md).

This design started as [`RuntimeMemory.cpp`](https://github.com/Igwe-Starking/eccles-esp32-smart-bike/blob/main/firmware/source/components/eccles/src/RuntimeMemory.cpp), a single ESP32-only file written for the [eccles-esp32-smart-bike](https://github.com/Igwe-Starking/eccles-esp32-smart-bike) project. The three-pool layout, the circular per-pool cursor, and the run-length registry are all the same ideas — this repo just generalizes the platform-specific parts (locking, RAM sizing) that the original hard-coded for one chip.

**Contents:** [How it works](#how-it-works) · [Configuring](#configuring) · [Locking](#locking-threadisr-safety) · [Free status codes](#what-eccles_rt_free-tells-you) · [Debug logging](#debug-logging) · [API](#api) · [Testing](#testing)

## How it works

One memory budget (`ECCLES_RT_MEM_SIZE` bytes) is split into three pools of
fixed block sizes — small (A), medium (B), large (C). `eccles_rt_malloc()`
tries the smallest class that fits the request first, then escalates to
progressively larger classes if that one has no free block right now (it
never steps down to a smaller class — its blocks might not be big enough).
Requests bigger than one large block need a contiguous multi-block run
instead, tried largest-class-first by default (see `ECCLES_RT_MALLOC_MIN_
WASTE` below for an alternative) — but every individual allocation still
only ever comes from *one* pool; blocks from two different classes are
never combined into a single allocation. `eccles_rt_free()` validates the
pointer (real block start? not already free? not the middle of a run?)
before touching any bookkeeping, and tells you *why* it rejected an
invalid one via its return value.

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

Defaults, if you don't configure anything, in priority order:

1. **You declared `ECCLES_RT_TOTAL_RAM_SIZE`** (your chip's actual RAM, in
   bytes) — the budget becomes a tiered percentage of it: 50% if <4 KB,
   25% if <10 KB, 10% if <100 KB, 5% otherwise. Smaller chips get a bigger
   slice (there's less of it competing for the space); bigger chips get a
   smaller one. Example: a Mega2560 (8 KB RAM) gets 2 KB instead of being
   stuck with the same 1 KB as a 2 KB-RAM Uno under the old flat rule.
   **Known rough edge**: there's a small non-monotonic step exactly at
   each boundary — 4095 bytes of RAM computes ~2048 bytes of budget (50%),
   but 4096 bytes computes only ~1024 (25%). If your part's RAM sits close
   to one of these lines, skip this and set `ECCLES_RT_MEM_SIZE` directly.
2. **Your chip is in the built-in lookup table** — a small, curated list of
   common chips (classic 8-bit Arduino/LaunchPad parts) whose compiler
   already defines an exact-RAM-size chip macro (e.g. `__AVR_ATmega328P__`)
   with no build-system cooperation needed. Same tiered-percentage math as
   above, just fed from the table instead of your declaration. **Not
   exhaustive** — add your own entry or use option 1 if your chip isn't in it.
3. **Coarse per-family fallback**, if neither of the above applies:

   | | RAM-constrained MCUs | everything else |
   |---|---|---|
   | detected via | `__AVR__`, `__MSP430__`, `__STM8__`, 8-bit PIC (`__XC8__`/`_PIC12`/`_PIC16`/`_PIC18`), 8051 (`__C51__`/`__CX51__`/SDCC-mcs51), entry-level Cortex-M0 STM32 (`STM32F0`/`G0`/`L0`/`C0`) | ESP32/ESP8266, bigger STM32 lines, SAMD, RP2040, nRF52, PIC32, desktop test builds, anything not in the left column |
   | total budget | 1 KB | 20 KB |
   | block sizes (A/B/C) | 16 / 32 / 64 bytes | 64 / 128 / 256 bytes |
   | split | 25% / 25% / 50% | 25% / 25% / 50% |

   This is a **family-level guess, not a per-chip one** — a given family
   can span a huge RAM range (an STM32F1 might have 4 KB or 128 KB
   depending on the exact part). Treat it as a convenience for
   prototyping only.

For anything you intend to ship, set `ECCLES_RT_MEM_SIZE` explicitly to a
number you've checked against your part's actual RAM, or at minimum set
`ECCLES_RT_TOTAL_RAM_SIZE` so the *proportion* is sensible even if the
absolute number wasn't hand-picked.

### Block sizes, split, and validation

Block sizes must be a multiple of `ECCLES_RT_ALIGNMENT` (see "Alignment"
below — otherwise blocks after the first in a class wouldn't actually land
on an aligned address) and must satisfy `A <= B <= C`, since
`eccles_rt_malloc()`'s size routing assumes that ordering. `ECCLES_RT_POOL_
A_PERCENT + ECCLES_RT_POOL_B_PERCENT` can't exceed 100. The budget must
also be large enough to give every class at least one real block without
exceeding what you asked for — `ECCLES_RT_MEM_SIZE=1` used to silently
reserve 112 bytes (112x over) rather than failing, since each class is
guaranteed a minimum of one block; now it's a compile-time error instead.
All of these are checked at **compile time** with a clear `#error` if
violated, rather than silently misbehaving or overflowing at runtime.

A power of two is **recommended but not required** for block sizes: it
lets the allocator use a bit shift instead of a division on the hot path
(real savings on an AVR without a hardware multiplier). A non-power-of-two
size (that's still a multiple of `ECCLES_RT_ALIGNMENT`) works correctly —
detected at `eccles_rtmem_init()` and handled with plain division for that
pool — just somewhat slower.

Each pool's block *count* also doubles as a "run length" value in a 1-byte
registry entry (255 is reserved to mean "middle of a multi-block run"), so
the combined block count across A+B+C can't exceed 255 — also a compile-time
`#error` if your settings would exceed it.

### Alignment

Every pointer `eccles_rt_malloc()` returns is aligned to `ECCLES_RT_ALIGNMENT`
bytes (default 8, must be a power of two), so it's safe to cast to a wider
type. In heap mode this is done manually — over-allocate by up to
`ECCLES_RT_ALIGNMENT - 1` extra bytes and round the pointer up — rather than
trusting `malloc()`'s own alignment guarantee, which the C standard only
promises up to `alignof(max_align_t)` (commonly 8 or 16 bytes); this makes
the guarantee hold even for a custom `ECCLES_RT_ALIGNMENT` larger than that.
In static mode it's applied via a GCC/Clang/Arm-Compiler alignment
attribute on the pool array. On another toolchain in static mode, align
the pool array yourself if you need this guarantee.

### Allocation strategy for oversized (multi-block) requests

A request bigger than one C block needs a contiguous run of blocks from a
single class. Two strategies, chosen at compile time:

- **Default (largest-class-first)**: try C, then B, then A. Tends to
  preserve the small classes' block-count capacity for their intended job
  — frequent small allocations — rather than spending it on one large
  request, at the cost of possibly wasting more bytes on that one request
  than a smaller-class choice would have.
- **`ECCLES_RT_MALLOC_MIN_WASTE`** (opt-in): try whichever class wastes the
  *fewest bytes* on this specific request first. With the default
  64/128/256-byte classes, a 513-byte request needs 3 C-blocks (768 bytes,
  255 wasted) by default, but 9 A-blocks (576 bytes, only 63 wasted) under
  this option — less waste on that call, but 9 of pool A's blocks are now
  unavailable to whatever else needed pool A for small, frequent
  allocations. On an exact tie (a size that's a common multiple of more
  than one block size), the larger class wins the tie.

There's no universally-correct choice here — minimizing bytes wasted per
call and preserving small-pool capacity for future allocations are
genuinely in tension, and which matters more depends on your workload
(mostly-large-transfer firmware vs. mixed small+occasional-large firmware).
Computing the waste for all 3 classes is O(1) either way (a handful of
divisions, done once per oversized-allocation call, not in a hot loop) —
this isn't a performance tradeoff, it's a "which failure mode do you want
to bias against" one.

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

Unlike a typical `free()`, this one returns an `eccles_rt_status_t` so you
can find out *why* an invalid free was rejected, not just that it was:

```c
typedef enum {
    ECCLES_RT_FREE_OK = 0,
    ECCLES_RT_FREE_NULL,          /* buffer was NULL - harmless no-op */
    ECCLES_RT_FREE_NOT_READY,     /* eccles_rtmem_init() hasn't run/succeeded yet */
    ECCLES_RT_FREE_OUT_OF_RANGE,  /* pointer isn't inside any pool this allocator owns */
    ECCLES_RT_FREE_MISALIGNED,    /* inside a pool, but not on a block boundary */
    ECCLES_RT_FREE_ALREADY_FREE,  /* that block is already marked free */
    ECCLES_RT_FREE_MID_RUN        /* pointer into the middle of a multi-block allocation */
} eccles_rt_status_t;
```

`ECCLES_RT_FREE_ALREADY_FREE` is deliberately not called `DOUBLE_FREE`
(which it was briefly named): the registry can only tell you a block is
*currently* marked free, not whether this is literally the same allocation
being freed twice versus a stale pointer that happens to collide with some
other, unrelated block that's free right now — those look identical from
here. `ECCLES_RT_FREE_DOUBLE_FREE` still works as a `#define` alias.

Existing code that just calls `eccles_rt_free(buf);` and ignores the return
value keeps working exactly as before.

**What this can't catch**: a pointer that lands on a real, currently-
allocated block boundary is accepted, even if it's actually a stale pointer
to an allocation that was already freed and reused by something else -
there's no per-block generation counter to detect that (most fixed-pool/
slab allocators without extra bookkeeping share this limitation; it isn't
unique to this one). Freeing a stale pointer in that situation silently
corrupts whatever's actually live there now instead of being caught.

## Debug logging

```c
#define ECCLES_RT_DEBUG          /* turns logging on */
/* optional: point it at your own output instead of the printf() default */
#define ECCLES_RT_LOG_LINE(msg) Serial.println(msg)
#include "eccles_rtmem.h"
```

Logs allocation failures and rejected frees — never anything on the success
path. Always logged *after* releasing the internal lock, never while it's
held — `ECCLES_RT_LOG_LINE` can be a slow I/O call (`Serial.println`,
`printf`, ...), which has no business running while interrupts are masked
or another task is blocked waiting on the same lock.

## API

```c
bool                eccles_rtmem_init(void);  /* call once; safe to call again (no-op) */
uint8_t*            eccles_rt_malloc(size_t size);
uint8_t*            eccles_rt_calloc(size_t count, size_t size);   /* zero-initialized, overflow-safe */
uint8_t*            eccles_rt_realloc(uint8_t *ptr, size_t new_size);
uint8_t*            eccles_rt_aligned_alloc(size_t alignment, size_t size); /* limited - see below */
eccles_rt_status_t  eccles_rt_free(uint8_t *buffer);
bool                eccles_rt_reset(void);     /* wipe every pool back to fully-free */
eccles_rt_stats_t   eccles_rt_get_stats(void); /* used/free block counts per pool */
```

`eccles_rtmem_init()` is **idempotent for repeated calls from the same
initializing context**: a second call is a safe no-op rather than
re-`malloc()`ing a fresh pool (heap mode) or otherwise disturbing buffers
you've already handed out. It returns `false` only if
`ECCLES_RT_MEM_USE_HEAP` is set and the underlying `malloc()` failed; you can
call it again later to retry. It is **not** safe to call from two contexts
at the same time — there's no lock protecting the init sequence itself,
since the lock doesn't exist yet until init creates it. Call it once, from
a single thread/task, before starting anything else that might also call it
or call `eccles_rt_malloc`/`_free`.

`eccles_rt_calloc`/`_realloc`/`_aligned_alloc` mirror the standard library's
contracts as closely as makes sense for a fixed-pool allocator:

- **`eccles_rt_calloc`** is `eccles_rt_malloc(count * size)` plus a
  `memset(0)`, with an overflow-safe multiply (returns `NULL` instead of
  wrapping around to a too-small allocation if `count * size` would overflow
  `size_t`).
- **`eccles_rt_realloc`** preserves the standard contract that matters most:
  on failure, the original pointer is **untouched and still valid** — a
  classically common bug in naive reallocation code is assuming a failed
  `realloc` already freed the old pointer. `realloc(NULL, size)` behaves
  like `malloc(size)`; `realloc(ptr, 0)` frees `ptr` and returns `NULL`.
  Resizing is done **in place** whenever possible, in one critical section:
  shrinking releases the trailing blocks back to the pool immediately,
  growing checks the boundary right after the run and absorbs the next
  blocks if they are free and inside the same pool — same pointer returned,
  no copy either way. Only when the neighbour is in use (or the pool ends)
  does it fall back to allocate-new + copy + free-old, which may move the
  allocation to a different class.
- **`eccles_rt_aligned_alloc`** is a **limited** equivalent, not a general
  one: every allocation is already aligned to `ECCLES_RT_ALIGNMENT`
  unconditionally, so this succeeds at zero extra cost whenever the
  requested alignment is already satisfied by that (≤ `ECCLES_RT_ALIGNMENT`
  and a power of two), and returns `NULL` for anything bigger. Supporting
  arbitrary alignments beyond the built-in one would need per-allocation
  offset bookkeeping this library deliberately doesn't carry. Raise
  `ECCLES_RT_ALIGNMENT` itself if you need a bigger guarantee everywhere.

`eccles_rt_reset()` wipes every pool's bookkeeping back to fully-free in one
call — every registry entry cleared, cursors and used-block counters reset
— without reallocating anything (heap mode reuses the same buffer) or
requiring another `eccles_rtmem_init()` call. It does **not** zero the
actual pool bytes (same "don't pay for zeroing nobody asked for" philosophy
as `eccles_rt_free()`), and it does **not** know or care that you might
still be holding pointers from before the reset — using or freeing one
afterward is exactly as unsafe as a use-after-free, possibly worse (the
address may now belong to a completely different live allocation). This is
meant for "start over, nothing else still depends on prior allocations"
situations (between test cases, a deliberate subsystem soft-reset), not as
a general "free everything I forgot to free" safety net.

## Testing

`tests/` has:
- **`test_unit.c`** — deterministic checks: class-boundary routing,
  small-allocation escalation (A→B→C when a class is full), multi-block
  allocation under both the default and `MIN_WASTE` strategies, mid-run/
  already-free/out-of-range/misaligned free rejection, pool exhaustion
  and reuse, a fragmentation scenario proven to fail correctly even with
  no fallback room, idempotent re-init, and `calloc`/`realloc`/
  `aligned_alloc`/`reset`.
- **`test_stress.c`** — randomized alloc/free with canary-byte corruption
  detection (every live buffer is stamped with a unique byte pattern and
  re-checked before every free) and `eccles_rt_get_stats()` invariant
  checks (`used + free == count`) throughout a run. Run with
  `./test_stress [op_count] [seed]`; defaults to 500,000 ops. Passes clean
  under both AddressSanitizer and UndefinedBehaviorSanitizer at up to 2M
  operations.
- **`test_smoke.c`** — a minimal, config-agnostic check safe for any valid
  configuration, including a 1-block-per-pool minimum.
- **`run_tests.sh`** — runs the above across 12 distinct configurations:
  default, heap mode, `NO_LOCK`, the `MIN_WASTE` strategy, a 1 KB
  weak-MCU-scale budget, a custom A/B/C split, custom block sizes, a
  near-the-255-block-cap configuration, a minimum-viable pool, and three
  exercising the RAM-tiered budget detection (a simulated ATmega328P and
  ATmega2560 via their chip-specific macros, and an explicit
  `ECCLES_RT_TOTAL_RAM_SIZE`).

```
make test        # or: cd tests && bash run_tests.sh
make sanitize    # same matrix under ASan + UBSan
```

This suite is how real bugs were found and fixed across every round of review -- see the [CHANGELOG](../CHANGELOG.md) for specifics.
