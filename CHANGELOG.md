# Changelog

All notable changes to this project are documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [Semantic Versioning](https://semver.org/).

## [2.0.0] - 2026-10-03

A full rewrite of the allocation core for speed, plus a smarter in-place `realloc`. No header renames and no removed symbols -- every 1.x call site keeps compiling and behaving the same on the happy path -- but the major version is bumped because two behaviors are now observably different: `realloc` can shrink/grow a pointer in place where 1.x always either took the no-op "fits already" path or copied, and the search order within a pool, while producing the same placement, no longer visits blocks one byte at a time (relevant only if you were somehow depending on timing or on the exact in-progress state mid-scan from another thread, which was never supported).

### Changed
- **`eccles_rt_getFree` is bitmap-driven.** A packed occupancy bitmap (at most 32 bytes, 1 bit per block) mirrors the block registry. The search jumps over runs of used or free blocks with one `__builtin_ctzll` per 64 blocks (a portable bit-by-bit fallback compiles on toolchains without the builtin) instead of testing registry bytes one at a time. Final placement is byte-for-byte identical to 1.x -- same circular first-fit order, same winning block -- this changes how fast the answer is found, not what the answer is.
- **O(1) full-pool rejection.** A pool with fewer free blocks than requested now returns immediately from a maintained `usedBlocks` counter instead of scanning. This is what makes the small-allocation escalation chain (A -> B -> C) from 1.1.0 cheap even when the earlier pools are completely full.
- **Single-block fast path.** For a 1-block request, once free capacity is proven to exist, the search cannot fail -- one `ctzll` in the common case, no run-length verification, no failure branch to fall through.
- **Word-wide bookkeeping.** Marking and clearing a run uses `memset` on the registry and whole-word masks on the bitmap instead of a per-block loop. Block-count math in `malloc`/`realloc` now goes through a shared shift+mask helper for power-of-two block sizes, so it costs no divide on AVR, MSP430, or Cortex-M0 (none of which have hardware division).
- **`eccles_rt_realloc` resizes in place.** 1.1.0's `realloc` only ever had a no-op fast path (new size fits the already-allocated capacity) or a full allocate+copy+free. 2.0.0 adds two more paths, both inside the same critical section as the resolve step so nothing else can claim the neighbouring blocks in between: shrinking releases the excess trailing blocks straight back to the pool; growing checks the boundary right after the run and absorbs the next blocks if they're free and still inside the same pool. Only when the neighbour is in use, or the pool ends, does it fall back to moving the allocation -- which, as before, may land in a different pool class.

### Added
- `test_realloc_in_place()`: grow-into-free-neighbour, shrink-releases-tail, same-block-count no-op, a blocked-neighbour case (must not stomp the block after it, whichever outcome it takes), and a pool-boundary case (last run in a full pool, nowhere to grow into). Runs as part of the existing 12-configuration matrix, including under AddressSanitizer and UndefinedBehaviorSanitizer.
- Additional stress-test verification for this release: 2,000,000-operation randomized runs (4x the suite's default) against the default config, the near-255-block config, and `MIN_WASTE`, all clean under ASan/UBSan with zero canary corruption and all pools empty at the end.

## [1.1.0] - 2026-09-28

A deep audit pass: RAM-aware sizing, three new allocation functions, and several correctness fixes. No breaking changes -- every renamed symbol keeps its old name as a `#define`/`typedef` alias.

### Added
- **RAM-aware default sizing.** The old flat 1 KB / 20 KB split was genuinely too coarse -- 1 KB is half the RAM on a 2 KB AVR but a rounding error on a 200 KB ESP32. Budget is now derived, in priority order, from: an explicit `ECCLES_RT_TOTAL_RAM_SIZE` you declare, or a built-in lookup table of common chips (classic 8-bit Arduino/LaunchPad parts, keyed off the compiler's own exact-RAM macro), both feeding a tiered percentage (50% under 4 KB, 25% under 10 KB, 10% under 100 KB, 5% otherwise); or the original coarse per-family guess as a last resort. Documented rough edge: a small non-monotonic step exactly at each tier boundary.
- **`eccles_rt_calloc()`, `eccles_rt_realloc()`, `eccles_rt_aligned_alloc()`.** Mirror the standard library's contracts as closely as a fixed-pool allocator allows -- overflow-safe `calloc`, a `realloc` that never invalidates the original pointer on failure, and an `aligned_alloc` limited to `ECCLES_RT_ALIGNMENT` (documented, not silently wrong).
- **`eccles_rt_reset()`.** Wipes every pool's bookkeeping back to fully-free in one call, without reallocating anything or requiring another `eccles_rtmem_init()`. Meant for "nothing else still depends on prior allocations" situations (between test cases, a deliberate subsystem soft-reset) -- it does not zero pool bytes and does not protect against use of pointers issued before the reset.
- **Small-allocation escalation.** A request that fits pool A used to only ever try pool A. It now tries the smallest class that fits and escalates upward (A -> B -> C) if that class is full -- symmetric with the existing oversized-request fallback.
- **`ECCLES_RT_MALLOC_MIN_WASTE`.** Opt-in alternate strategy for oversized (multi-block) requests: picks the class that wastes the fewest bytes on that specific request, instead of always trying the largest class first. Off by default, since minimizing per-call waste and preserving small-pool capacity for future allocations are genuinely in tension -- see the README for a worked example.
- Three more test configurations (12 total): the `MIN_WASTE` strategy, and RAM-tiered detection for a simulated ATmega328P, ATmega2560, and an explicit `ECCLES_RT_TOTAL_RAM_SIZE`.

### Fixed
- **`ECCLES_RT_MEM_SIZE` could silently be violated.** A budget too small to give every class its guaranteed minimum of one block used to reserve *more* bytes than asked for instead of failing -- `ECCLES_RT_MEM_SIZE=1` with default block sizes silently reserved 112 bytes. Now a compile-time `#error`.
- **Alignment wasn't actually guaranteed past the first block in a class smaller than `ECCLES_RT_ALIGNMENT`.** Verified: block 2 of a 4-byte class landed 4 bytes off an 8-byte alignment. Fixed with a compile-time check that every block size is a multiple of `ECCLES_RT_ALIGNMENT`, plus a portable heap-mode fix (manual over-allocate + round up).
- **A failed RTOS mutex/semaphore creation went unreported.** If `ECCLES_RT_LOCK_INIT()` failed (e.g. FreeRTOS's heap exhausted), `eccles_rtmem_init()` used to still report success, after which every call silently no-op'd with no indication why. Now checked and reported (`eccles_rtmem_init()` returns `false`).
- **Overly broad ARM lock detection.** The generic-ARM branch matched any `__arm__`/`__ARM_ARCH` target and used Cortex-M-only PRIMASK instructions unconditionally -- a false match on Cortex-A/R or older ARM7TDMI cores would have meant an invalid instruction. Narrowed to actual Cortex-M architecture macros; anything else now falls through to the no-op fallback.
- **Debug logging used to run inside the lock.** A slow `ECCLES_RT_LOG_LINE` call (`Serial.println`, `printf`, ...) blocked every other task/ISR waiting on the same lock for its whole duration. Moved to after unlock in both `eccles_rt_malloc` and `eccles_rt_free`.
- **`eccles_rt_get_stats()` used to rescan the whole registry (up to 255 entries) under the lock on every call.** Replaced with running counters maintained incrementally by malloc/free, making stats O(1) and shortening the locked section -- verified against 4M+ stress-test operations with zero drift.
- `#warning` on an unset `ECCLES_RT_MEM_SIZE` removed -- GCC/Clang classify it as a warning diagnostic, so `-Werror` (ESP-IDF's/PlatformIO's stricter presets) turned a friendly reminder into a hard build failure. The guidance still lives in comments.
- Documentation corrected: block sizes were documented as "must be a power of two" when the code already tolerates (and is tested against) any positive multiple of `ECCLES_RT_ALIGNMENT`, just slower via division.

### Changed
- `eccles_rt_free_status_t` renamed to `eccles_rt_status_t` (old name kept as a `typedef` alias).
- `ECCLES_RT_FREE_DOUBLE_FREE` renamed to `ECCLES_RT_FREE_ALREADY_FREE`, since the registry can only tell you a block is *currently* marked free, not whether this is literally the same allocation being freed twice versus an unrelated stale pointer -- the old name implied a certainty the check can't provide. Old name kept as a `#define` alias.
- `eccles_rtmem_init()`'s idempotency claim is now precise: safe for repeat calls from the same initializing context, not for concurrent calls from two contexts at once (there's no lock protecting init itself, since the lock doesn't exist until init creates it).

## [1.0.0] - 2026-09-24

First public release. Portable plain-C rewrite of the original ESP-IDF-only allocator, hardened through a review pass.

### Added
- Portable C99 implementation (`src/eccles_rtmem.c` / `.h`) with three fixed-size pools (A/B/C), sized from `ECCLES_RT_MEM_SIZE` and block-size/percent macros.
- Optional heap-backed pool (`ECCLES_RT_MEM_USE_HEAP`).
- MCU-family detection for the default memory budget (1 KB vs. 20 KB).
- Automatic locking backends: Zephyr, FreeRTOS, CMSIS-RTOS2, Pico SDK, AVR, MSP430, Microchip XC16/XC32, generic Cortex-M; `ECCLES_RT_MEM_NO_LOCK` and a manual override.
- `eccles_rt_free()` returns a status enum, reporting *why* a pointer was rejected.
- `eccles_rt_get_stats()` for per-pool used/free block counts.
- Optional debug logging (`ECCLES_RT_DEBUG`, `ECCLES_RT_LOG_LINE`).
- `eccles_rtmem_config.h` configuration template picked up via `__has_include`.
- Compile-time validation of block sizes, pool percentages and total block count.
- Test suite (unit, stress with canary corruption detection, smoke) run across 8 configurations.
- Examples for Arduino AVR, Arduino ESP32, ESP-IDF, STM32 (bare-metal and FreeRTOS), RP2040, nRF52/Zephyr, Nano 33 BLE/mbed, MSP430, and a lock-free super-loop.

### Fixed (relative to pre-release development builds)
- Out-of-bounds pointer arithmetic in `eccles_rt_free()` for garbage input pointers; range checks now go through `uintptr_t`.
- Out-of-bounds write in the circular cursor search when a pool's absolute end index was near 256 (8-bit truncation before the wraparound check). Found by the near-255-block test configuration under AddressSanitizer.
- `eccles_rt_malloc(0)` now returns `NULL` instead of reserving a block.
- `eccles_rtmem_init()` is idempotent and returns `bool`.
- Interrupt-mask lock backends save state in a call-site-local variable instead of a shared static, making them safely reentrant.
- Compile-time checks prevent `POOL_A_PERCENT + POOL_B_PERCENT > 100` silently underflowing pool C.
- `size_t` instead of `uint32_t` for sizes and offsets (faster on 8/16-bit targets).
- Returned pointers are aligned to `ECCLES_RT_ALIGNMENT` (default 8) in static mode on GCC/Clang.

### Renamed
- `e_malloc`, `e_free`, `e_getStats`, `e_PoolStats`, `initRuntimeMemory` are now `eccles_rt_malloc`, `eccles_rt_free`, `eccles_rt_get_stats`, `eccles_rt_stats_t`, `eccles_rtmem_init`. The old names remain as `#define` aliases unless `ECCLES_RT_NO_DEPRECATED_ALIASES` is defined.
