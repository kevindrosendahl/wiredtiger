# Implementation Review: `PLAN-startup-config-parser-perf.md`

**Review Date**: 2026-02-17  
**Reviewer**: Implementation Reviewer (isolated)  
**Design Document**: `PLAN-startup-config-parser-perf.md`  
**Implementation Scope Reviewed**:
- `src/config/config_collapse.c`
- `src/schema/schema_create.c`
- `src/conn/conn_dhandle.c`
- `src/include/extern.h`
- `src/include/stat.h`
- `src/include/wiredtiger.h.in`
- `src/support/stat.c`
- `dist/stat_data.py`
- `test/catch2/misc_tests/test_config.cpp`

## Executive Summary

The implementation has materially improved versus the previous review:

- WS1 is now correctly routed through `schema_create.c` call sites instead of globally replacing `__wt_config_collapse`.
- WS2 reuse gating is correctly implemented with normalized metadata lookup, `dhandle->cfg[1]` precondition, and strict full metadata-string equality.
- Connection-level observability counters for WS1 and WS2 were added and wired through stats generation.

No critical correctness, memory-safety, or thread-safety regressions were found in the reviewed deltas.

| Category | Issues Found |
|---|---|
| Critical correctness | 0 |
| Memory safety | 0 |
| Thread safety | 0 |
| Medium | 3 |
| Minor | 0 |

## Findings (ordered by severity)

### [MEDIUM] New WS2 counter test is not hermetic and leaves artifacts in the repository working directory

- The new test uses fixed paths and names:
  - DB home: `test_db_ws2_dhandle`
  - URI: `table:ws2_config_reuse`
- Catch2 cleanup helper (`utils::wiredtiger_cleanup`) only removes a fixed allowlist of filenames and does not remove arbitrary table files like `ws2_config_reuse.wt`.
- Result: repeated local runs leave files behind and produce noisy warnings (`unexpected file ... renamed`), reducing test isolation.

**Recommendation**: make the test self-cleaning by using a cleanup-friendly table name, extending cleanup for this test artifact, or using a unique per-run DB home that is fully removed.

### [MEDIUM] WS2 reuse-hit assertion can pass without proving verify-specific reuse behavior

- In `test_config.cpp`, the test checks:
  - `hits_after_verify > hits_before`
- This can pass even if verify does not increase hits, as long as an earlier operation in the same test already increased `dhandle_config_reuse_hits`.
- The intended guarantee in the comment is stronger: verify itself should exercise reuse-hit behavior on unchanged metadata.

**Recommendation**: capture `hits_after_create` (or pre-verify hits) and assert `hits_after_verify > hits_after_create`.

### [MEDIUM] Planned WS2 rollout safety coverage is still incomplete

- The design calls out additional high-value coverage that is not yet present in this change set:
  - deterministic failure-injection retry between config allocation and metadata assignment,
  - disaggregated stable-URI lookup validation,
  - tiered metadata churn reopen checks (`last`/`tiers` forcing rebuild),
  - concurrency/race coverage for alter/checkpoint/open interactions.
- Current tests validate basic parity/counter behavior but do not yet close these higher-risk edges.

**Recommendation**: add the planned WS2 edge-case tests before considering rollout complete.

## Positive Notes

- `__wt_config_collapse` remains legacy behavior; startup call-site routing uses `__wti_config_collapse_fast`, matching WS1 rollout scope.
- WS1 fast-path fallback behavior for dotted projected keys is implemented and parity-tested.
- WS2 helper split improves ownership handling (`metadata_lookup`, `config_set_from_meta`, `meta_base_build`) and avoids partial ownership transfer on failure.
- Rebuild path still clears btree config cache through existing `__conn_dhandle_config_clear` semantics.
- Stats plumbing is complete across `dist/stat_data.py`, `stat.h`, `stat.c`, and `wiredtiger.h.in`.

## Validation Performed

- Built and ran targeted config tests:
  - `ninja -C build catch2-unittests`
  - `./build/test/catch2/catch2-unittests "[config]"`
- Result: tests passed (61 assertions, 3 test cases).

## Overall Recommendation

**Improved and close, but not yet fully rollout-ready per design guardrails.**

Core implementation changes are in good shape and no critical regressions were found. The remaining blockers are mostly test robustness and planned WS2 edge-case coverage.
