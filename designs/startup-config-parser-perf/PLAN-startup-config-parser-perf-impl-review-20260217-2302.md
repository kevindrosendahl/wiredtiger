# Implementation Review: `PLAN-startup-config-parser-perf.md`

**Review Date**: 2026-02-17  
**Reviewer**: Implementation Reviewer (isolated)  
**Design Document**: `PLAN-startup-config-parser-perf.md`  
**Implementation Scope Reviewed**:
- `src/config/config_collapse.c`
- `src/conn/conn_dhandle.c`

## Executive Summary

No correctness-critical regressions were found in the current implementation deltas.
WS2 metadata reuse gating is implemented safely (`dhandle->cfg[1]` precondition, strict full metadata-string equality, normalized metadata lookup), and ownership/error paths are generally sound.

However, the implementation is not yet complete against the approved plan and is missing key rollout guardrails (scope control, observability, and tests) needed to validate performance and parity.

| Category | Issues Found |
|---|---|
| Critical correctness | 0 |
| Memory safety | 0 |
| Thread safety | 0 |
| Medium | 4 |
| Minor | 0 |

## Findings (ordered by severity)

### [MEDIUM] WS1 optimization scope is broader than the design and can impact non-startup callers

- `__wt_config_collapse` now unconditionally attempts `__config_collapse_fast` before the legacy implementation.
- The design scopes WS1 rollout to startup-heavy schema call sites first (routing in `schema_create.c`), not global activation.
- This now affects all collapse callers (`meta_ckpt.c`, `tiered_handle.c`, `conn_layered.c`, etc.), including paths where fast-path eligibility may be low.
- For unsupported inputs (for example dotted projected keys), the current code first parses in fast path and then reparses in legacy fallback, adding overhead outside targeted startup flows.

**Recommendation**: keep fast path limited to startup call-site routing (or behind an internal runtime switch) until parity/perf confidence is established.

### [MEDIUM] WS2 rebuild-path parser reduction is not implemented yet

- `__conn_dhandle_meta_base_build` still uses the legacy `collapse + merge(strip)` chain.
- WS2 goals include reducing rebuild-path parser work via a dedicated metadata-base builder.
- The current patch improves reuse-hit behavior, but metadata-change rebuild path still pays legacy parser cost.

**Recommendation**: implement the dedicated base builder with deterministic fallback, or explicitly mark WS2 as partial and avoid claiming rebuild-path parser reductions yet.

### [MEDIUM] Required WS1/WS2 observability counters are missing

- The plan defines WS1/WS2 counters and acceptance thresholds (attempt/hit/fallback, reuse-hit/rebuild).
- No stats plumbing changes are present (for example no `dist/stat_data.py` updates).
- Without counters, rollout guardrails and hit-rate targets cannot be validated.

**Recommendation**: add permanent connection-level counters for WS1 and WS2 before merge.

### [MEDIUM] Planned test coverage is not implemented in this change set

- No tests were added with these source changes.
- Missing planned coverage includes:
  - WS1 parity tests (duplicate projected keys, quoted forms, dotted-key fallback parity).
  - WS2 deterministic failure-injection retry between config allocation and metadata assignment.
  - WS2 disaggregated stable-URI lookup validation.
  - WS2 tiered metadata churn reopen behavior (`last`/`tiers`).
  - WS2 concurrency/race coverage for alter/checkpoint/open interactions.

**Recommendation**: add targeted Catch2/csuite/suite tests before rollout readiness.

## Positive Notes

- Metadata lookup normalization and `WT_NOTFOUND` to `ENOENT` mapping are preserved via `__conn_dhandle_metadata_lookup`.
- Reuse gating correctly requires initialized cached metadata (`dhandle->cfg[1] != NULL`) plus strict full metadata-string equality.
- `__conn_dhandle_config_set_from_meta` improves ownership handling by staging allocations locally and transferring into `dhandle` only after full success.

## Overall Recommendation

**Not ready for merge as a complete implementation of the plan.**

Reason: no critical correctness issue was found in reviewed code, but key plan guardrails (scope control, observability, and tests) remain incomplete.

## Files Reviewed

- `src/config/config_collapse.c`
- `src/conn/conn_dhandle.c`
- `src/config/config.c`
- `src/meta/meta_ckpt.c`
- `designs/startup-config-parser-perf/PLAN-startup-config-parser-perf.md`
