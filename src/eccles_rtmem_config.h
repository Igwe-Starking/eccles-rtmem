/*
    eccles_rtmem_config.h - configuration template for EcclesRTLib
    ------------------------------------------------------------------
    Every setting below is commented out, meaning eccles_rtmem.h's own
    defaults apply. Uncomment (remove the leading "//") whichever ones you
    actually want to change, and delete or leave alone the rest.

    WHY THIS FILE: eccles_rtmem.c includes eccles_rtmem.h too, so your
    application code and the library's own .c file both need to see
    IDENTICAL configuration macros, or they'll disagree about how big the
    pools are. Putting your settings here instead of above an #include in
    your own source file solves that: this file is picked up automatically
    (via __has_include) by every file that includes eccles_rtmem.h,
    including eccles_rtmem.c itself, as long as it's placed somewhere your
    compiler's include search path reaches (the simplest option: right
    next to eccles_rtmem.h/.c).

    If you don't need to change anything, you don't need this file at all
    - delete it and every default in eccles_rtmem.h applies as-is.
*/
#ifndef ECCLES_RTMEM_CONFIG_H
#define ECCLES_RTMEM_CONFIG_H

/* ===================================================================== *
 *  memory budget
 * ===================================================================== */

/* Total bytes given to the three pools combined. Auto-detected from your
   MCU family if you leave this commented out (1 KB on RAM-constrained
   families like AVR/MSP430/STM8/8-bit PIC/8051/entry Cortex-M0 STM32,
   20 KB otherwise) - see the ECCLES_RT_MEM_SIZE note in eccles_rtmem.h
   for why you should set this explicitly for anything you intend to ship,
   rather than relying on that guess. */
// #define ECCLES_RT_MEM_SIZE        4096ul

/* Uncomment to malloc() the pool + registry once inside eccles_rtmem_init()
   instead of using static (.bss) arrays. Leave commented out (static) if
   you're not sure - it's the more predictable choice on a microcontroller. */
// #define ECCLES_RT_MEM_USE_HEAP

/* Byte alignment guaranteed for every pointer eccles_rt_malloc() returns.
   Default 8. Only matters if you'll cast the returned pointer to a wider
   type (e.g. uint32_t*). Automatic in heap mode; applied via a GCC/Clang
   attribute in static mode - see the ECCLES_RT_ALIGNMENT note in
   eccles_rtmem.h if you're on a non-GCC/Clang toolchain. */
// #define ECCLES_RT_ALIGNMENT       8

/* ===================================================================== *
 *  block sizes and pool split
 * ===================================================================== */

/* Block size (bytes) of each of the 3 pools: A (small) / B (medium) /
   C (large). Must all be powers of two, and satisfy A <= B <= C - both
   checked at compile time. Default: 16/32/64 if ECCLES_RT_MEM_SIZE is
   small (<=4096), else 64/128/256. If you set one of these, you'll
   usually want to set all three together. */
// #define ECCLES_RT_BLOCK_A_SIZE    16ul
// #define ECCLES_RT_BLOCK_B_SIZE    32ul
// #define ECCLES_RT_BLOCK_C_SIZE    64ul

/* Percent of ECCLES_RT_MEM_SIZE given to pool A and pool B; pool C always
   gets whatever's left over. Their sum can't exceed 100 (checked at
   compile time). Default: 25 / 25 (so C gets ~50%). */
// #define ECCLES_RT_POOL_A_PERCENT  25ul
// #define ECCLES_RT_POOL_B_PERCENT  25ul

/* ===================================================================== *
 *  locking
 * ===================================================================== */

/* Uncomment to strip out ALL locking - no mutex, no interrupt masking.
   Only safe if eccles_rt_malloc/_free/_get_stats will only ever be called
   from a single thread/task/ISR context, or you're doing your own
   coarser-grained locking around whole sequences of calls yourself. This
   wins over everything else in this section, including a manual override
   below. */
// #define ECCLES_RT_MEM_NO_LOCK

/* Force the FreeRTOS locking path even if eccles_rtmem.c's auto-detection
   doesn't spot FreeRTOS on its own (it already checks for ESP_PLATFORM,
   INC_FREERTOS_H, and ARDUINO_ARCH_ESP32 automatically - this is only for
   the rare case none of those are defined in your build but FreeRTOS is
   genuinely there). */
// #define ECCLES_RT_USE_FREERTOS

/* Uncomment to skip the CMSIS-RTOS2 auto-detection (eccles_rtmem.c looks
   for a reachable cmsis_os2.h and uses osMutex if it finds one - define
   this if that header happens to be on your include path for unrelated
   reasons and you don't actually want CMSIS-RTOS2 locking). */
// #define ECCLES_RT_NO_CMSIS_RTOS2

/* Manual override: define ALL FIVE of these together to plug in your own
   mutex/critical-section instead of any auto-detected one (any RTOS, any
   chip). `m` is the single shared mutex object (ECCLES_RT_MUTEX_T); `st`
   is a variable of type ECCLES_RT_LOCK_STATE_T declared fresh at each
   call site (not a shared static) - this is what makes interrupt-mask-
   style locking correctly reentrant. If your backend doesn't need saved
   state (a real RTOS mutex, for instance), just ignore `st` in your
   macros and give ECCLES_RT_LOCK_STATE_T a throwaway type like uint8_t.
   Template below (commented out) shows the shape, using a made-up
   "myrtos_mutex_lock/unlock" API as a stand-in for a real one: */
// #define ECCLES_RT_MUTEX_T          myrtos_mutex_t
// #define ECCLES_RT_LOCK_STATE_T     uint8_t
// #define ECCLES_RT_LOCK_INIT()      myrtos_mutex_create()
// #define ECCLES_RT_LOCK(m, st)      (myrtos_mutex_lock(m), (void)(st))
// #define ECCLES_RT_UNLOCK(m, st)    (myrtos_mutex_unlock(m), (void)(st))

/* ===================================================================== *
 *  debug logging
 * ===================================================================== */

/* Uncomment to log allocation failures and rejected frees (double-free,
   mid-run pointer, non-block-boundary pointer, etc.) - never anything on
   the success path. Defaults to printf() if you don't also define
   ECCLES_RT_LOG_LINE yourself. */
// #define ECCLES_RT_DEBUG

/* Point logging at your own output instead of the printf() default, e.g.
   on Arduino: */
// #define ECCLES_RT_LOG_LINE(msg)   Serial.println(msg)

/* ===================================================================== *
 *  misc
 * ===================================================================== */

/* Uncomment if you don't want the old pre-review names (initRuntimeMemory,
   e_malloc, e_free, e_getStats, e_PoolStats) available as aliases for the
   current ones (eccles_rtmem_init, eccles_rt_malloc, eccles_rt_free,
   eccles_rt_get_stats, eccles_rt_stats_t). */
// #define ECCLES_RT_NO_DEPRECATED_ALIASES

#endif /* ECCLES_RTMEM_CONFIG_H */
