# Contributing to EcclesRTLib

Thanks for helping! Bug reports, new platform backends, docs fixes and tests are all welcome.

## Ground rules

- **Plain C99, no dependencies.** The library is two files (`src/eccles_rtmem.c`, `.h`). Keep it that way.
- **Deterministic and bounded.** No hidden heap use (except the opt-in heap mode), no unbounded loops, no recursion.
- **Everything configurable at compile time**, with a clear `#error` when a combination is invalid.
- **Every behavior change comes with a test.**

## Dev loop

```sh
make test       # 8-configuration matrix (default, heap, NO_LOCK, 1 KB, custom splits, near-255 cap, minimum pool)
make sanitize   # same matrix under AddressSanitizer + UndefinedBehaviorSanitizer
```

Use `CC=clang make test` to try another compiler.

Both must pass before you open a PR. CI runs them with gcc, clang and the sanitizers.

## Adding a locking backend for a new platform

1. Add a detection branch in the locking section of `src/eccles_rtmem.c`, placed in the priority order documented in [docs/DESIGN.md](docs/DESIGN.md#locking).
2. Give it a call-site-local `ECCLES_RT_LOCK_STATE_T` if it saves interrupt state.
3. Add an example under `examples/` and a row to the platform table in the README.
4. Say in the PR which real chip and toolchain you verified it on. Hardware-verified backends are worth far more than untested ones.

## Adding a test configuration

Add a `run "label" -DECCLES_RT_...` line to `tests/run_tests.sh`. Configs too small for `test_unit.c` can use `run_smoke`.

## Commit messages

Short imperative subject line ("Fix cursor wrap in pool C search"), details in the body if needed.
