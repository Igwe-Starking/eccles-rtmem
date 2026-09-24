# Changelog

All notable changes to this project are documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [Semantic Versioning](https://semver.org/).

## [1.0.0] - 2026-09-24

First public release. Portable plain-C rewrite of the original ESP-IDF-only allocator, hardened through a review pass.

### Added
- Portable C99 implementation (`src/eccles_rtmem.c` / `.h`) with three fixed-size pools (A/B/C), sized from `ECCLES_RT_MEM_SIZE` and block-size/percent macros.
- Optional heap-backed pool (`ECCLES_RT_MEM_USE_HEAP`).
- MCU-family detection for the default memory budget (1 KB vs. 20 KB).
- Automatic locking backends: Zephyr, FreeRTOS, CMSIS-RTOS2, Pico SDK, AVR, MSP430, Microchip XC16/XC32, generic Cortex-M; `ECCLES_RT_MEM_NO_LOCK` and a manual override.
- `eccles_rt_free()` returns `eccles_rt_free_status_t`, reporting *why* a pointer was rejected.
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
