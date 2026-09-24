#!/bin/sh
# Runs the test suite across several distinct configurations (default,
# heap mode, NO_LOCK, custom block sizes/percentages, a near-maximum
# block count, and a minimum-viable pool) - addresses the review item
# "test multiple configurations, not only the default".
#
# Usage: ./run_tests.sh
cd "$(dirname "$0")" || exit 1

CC=${CC:-gcc}
CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -I../src ${EXTRA_CFLAGS:-}"
FAILED=0

# run LABEL [DEFINES...] -- runs test_unit.c + test_stress.c (the full suite)
run() {
    label="$1"; shift
    echo "=== $label ==="
    tmpdir=$(mktemp -d)
    log="$tmpdir/build.log"

    if ! $CC $CFLAGS "$@" ../src/eccles_rtmem.c test_unit.c -o "$tmpdir/unit" > "$log" 2>&1; then
        echo "BUILD FAILED (unit):"; grep -v "not set\|C2X\|#warning \"eccles" "$log"
        FAILED=1; rm -rf "$tmpdir"; echo; return
    fi
    grep -v "not set\|C2X\|#warning \"eccles" "$log" || true
    if ! "$tmpdir/unit"; then
        echo "UNIT TESTS FAILED"; FAILED=1; rm -rf "$tmpdir"; echo; return
    fi

    if ! $CC $CFLAGS -O2 "$@" ../src/eccles_rtmem.c test_stress.c -o "$tmpdir/stress" > "$log" 2>&1; then
        echo "BUILD FAILED (stress):"; grep -v "not set\|C2X\|#warning \"eccles" "$log"
        FAILED=1; rm -rf "$tmpdir"; echo; return
    fi
    if ! "$tmpdir/stress" 200000 4242; then
        echo "STRESS TEST FAILED"; FAILED=1
    fi

    rm -rf "$tmpdir"
    echo
}

# run_smoke LABEL [DEFINES...] -- runs only test_smoke.c, for configs too
# small/irregular for test_unit.c's boundary-focused assumptions
run_smoke() {
    label="$1"; shift
    echo "=== $label (smoke test only) ==="
    tmpdir=$(mktemp -d)
    log="$tmpdir/build.log"

    if ! $CC $CFLAGS "$@" ../src/eccles_rtmem.c test_smoke.c -o "$tmpdir/smoke" > "$log" 2>&1; then
        echo "BUILD FAILED (smoke):"; grep -v "not set\|C2X\|#warning \"eccles" "$log"
        FAILED=1; rm -rf "$tmpdir"; echo; return
    fi
    grep -v "not set\|C2X\|#warning \"eccles" "$log" || true
    if ! "$tmpdir/smoke"; then
        echo "SMOKE TEST FAILED"; FAILED=1
    fi

    rm -rf "$tmpdir"
    echo
}

run "default (20 KB, 64/128/256, 80/40/40)"

run "heap mode" -DECCLES_RT_MEM_USE_HEAP

run "NO_LOCK" -DECCLES_RT_MEM_NO_LOCK

run "weak-MCU-scale budget (1 KB) on host's default no-op lock fallback" \
    -DECCLES_RT_MEM_SIZE=1024ul -DECCLES_RT_BLOCK_A_SIZE=16ul -DECCLES_RT_BLOCK_B_SIZE=32ul -DECCLES_RT_BLOCK_C_SIZE=64ul

run "custom ratio (A gets 60%, small pool)" \
    -DECCLES_RT_MEM_SIZE=8192ul -DECCLES_RT_POOL_A_PERCENT=60ul -DECCLES_RT_POOL_B_PERCENT=20ul

run "custom block sizes (non-default powers of two)" \
    -DECCLES_RT_MEM_SIZE=4096ul -DECCLES_RT_BLOCK_A_SIZE=32ul -DECCLES_RT_BLOCK_B_SIZE=128ul -DECCLES_RT_BLOCK_C_SIZE=512ul

run "near-maximum block count (251 total, close to the 255 cap)" \
    -DECCLES_RT_MEM_SIZE=4096ul -DECCLES_RT_BLOCK_A_SIZE=8ul -DECCLES_RT_BLOCK_B_SIZE=16ul -DECCLES_RT_BLOCK_C_SIZE=32ul \
    -DECCLES_RT_POOL_A_PERCENT=24ul -DECCLES_RT_POOL_B_PERCENT=25ul

run_smoke "minimum viable pool (1 block per class)" \
    -DECCLES_RT_MEM_SIZE=48ul -DECCLES_RT_BLOCK_A_SIZE=8ul -DECCLES_RT_BLOCK_B_SIZE=16ul -DECCLES_RT_BLOCK_C_SIZE=32ul \
    -DECCLES_RT_POOL_A_PERCENT=16ul -DECCLES_RT_POOL_B_PERCENT=33ul

if [ "$FAILED" = "1" ]; then
    echo "SOME CONFIGURATIONS FAILED"
    exit 1
else
    echo "ALL CONFIGURATIONS PASSED"
fi
