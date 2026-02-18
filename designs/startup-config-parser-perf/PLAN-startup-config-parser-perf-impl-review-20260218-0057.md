# Implementation Review: `PLAN-startup-config-parser-perf.md`

**Review Date**: 2026-02-18  
**Reviewer**: Implementation Reviewer (isolated)  
**Design Document**: `PLAN-startup-config-parser-perf.md`  
**Implementation Scope Reviewed**:
- `src/config/config_collapse.c`
- `src/schema/schema_create.c`
- `src/conn/conn_dhandle.c`
- `src/include/connection.h`
- `src/include/extern.h`
- `src/include/stat.h`
- `src/include/wiredtiger.h.in`
- `src/support/stat.c`
- `dist/stat_data.py`
- `test/catch2/misc_tests/test_config.cpp`

## Executive Summary

The branch has progressed substantially and now includes:

- WS1 call-site routing through `__wti_config_collapse_fast` in `schema_create.c`.
- WS2 reuse gating with normalized metadata lookup, strict metadata-string equality, and reuse/rebuild counters.
- New WS2-focused tests (reuse/rebuild counters, deterministic failpoint retry, concurrency stress, and helper checks).

Targeted config unit tests pass locally:

- `ninja -C build catch2-unittests`
- `./build/test/catch2/catch2-unittests "[config]"`
- Result: **all passed** (188 assertions, 7 test cases).

No memory-safety or thread-safety regressions were observed in the reviewed delta, but one medium-severity semantic risk remains in WS2 base derivation.

| Category | Issues Found |
|---|---|
| Critical correctness | 0 |
| Memory safety | 0 |
| Thread safety | 0 |
| Medium | 1 |
| Minor | 0 |

## Findings (ordered by severity)

### [MEDIUM] WS2 optimized base builder skips required collapse-equivalent canonicalization stage and can diverge from legacy semantics

- In `__conn_dhandle_meta_base_build`, the optimized path directly runs `__wt_config_merge(metaconf, strip)` (plus tiered `live_restore=` adjustment), without a collapse-equivalent pass over `metaconf` before stripping.
- The approved design locks this as a required two-step transform: collapse-equivalent canonicalization first, then strip.
- This matters for duplicate top-level struct keys: merge semantics recursively union nested fields, while collapse semantics are top-level last-wins replacement. Under such inputs, optimized output can differ from legacy output, which can change `meta_base`/`meta_hash` behavior.

**Recommendation**:

- Reintroduce a collapse-equivalent canonicalization stage (or an equivalent dedicated implementation) before strip in the optimized path.
- Add a diagnostic parity assertion (optimized vs legacy base) with fallback to legacy on mismatch until parity confidence is complete.

## Positive Notes

- `__wt_config_collapse` remains the legacy implementation; WS1 optimization is correctly routed through startup-heavy schema call sites.
- WS1 observability counters are fully wired (`dist/stat_data.py`, `stat.h`, `stat.c`, `wiredtiger.h.in`).
- WS2 ownership/error handling in `__conn_dhandle_config_set_from_meta` is cleaner and now supports deterministic failpoint injection for retry-path testing.
- Reuse gating correctly requires initialized cached metadata (`dhandle->cfg[1]`) and strict full metadata-string equality.

## Overall Recommendation

**Close, but not fully parity-safe yet.**

Address the WS2 base-derivation semantic gap above (and add parity guardrails around it) before considering this implementation fully aligned with the approved design.
