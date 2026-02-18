# Review: Startup Config Parser Performance Plan

Design doc reviewed: `PLAN-startup-config-parser-perf.md`  
Review date: 2026-02-17

## Critical Issues

### 1) WS2 metadata fetch path must preserve disaggregated URI normalization

The design introduces a new "fetch metadata once and compare" path but does not explicitly require the existing dhandle-name normalization step.

In current code, `__conn_dhandle_config_set` adjusts the metadata key via `__wt_btree_shared_base_name` before calling `__wt_metadata_search`. That normalization is required for names like `*.wt_stable/<checkpoint-id>`.

If the new metadata-fetch/change-detection logic skips this normalization, the metadata lookup can miss or read the wrong key, leading to open failures or incorrect reuse decisions for disaggregated handles.

**Required change to design**: make normalized-name lookup an explicit invariant of both the first-build and reuse-check paths (shared helper, not duplicated ad hoc).

### 2) Reuse check is unsafe unless it treats partially initialized `dhandle->cfg` as invalid

WS2 currently says "if previously built `dhandle->cfg` exists, compare metadata and skip rebuild when unchanged."  
That is not sufficient as a validity check.

`__conn_dhandle_config_set` can fail after allocating `dhandle->cfg` but before assigning `dhandle->cfg[1]` (metadata string). A later open retry that assumes "cfg exists => cfg is usable" can dereference `cfg[1]` when it is `NULL` or invalid.

**Required change to design**: define the reuse precondition as `dhandle->cfg != NULL && dhandle->cfg[1] != NULL` (and only then compare strings). Otherwise force full clear/rebuild.

### 3) The "normalized-base equality" option is unsafe for tiered handles

The open question asks whether reuse should be based on strict metadata-string equality or normalized-base equality.  
Normalized-base equality is not safe for tiered handles.

Current code strips `last` and `tiers` from `meta_base`, but tiered open reads these values from full metadata (`dhandle->cfg`) to determine object topology/state. If reuse is keyed only on normalized base, updates to `last/tiers` can be missed and stale tiered state reused.

**Required change to design**: require strict full metadata-string equality for reuse gating. Keep normalized-base equality out of scope unless tiered semantics are redesigned and proven safe.

## Medium Issues

### 1) Base-builder replacement is underspecified relative to current strip semantics

Current base derivation uses `collapse + merge(strip)` with specific strip sets by handle type. The proposed "dedicated metadata-base builder" says it removes known transient keys, but does not define exact behavior for nested/quoted/edge forms or future metadata keys.

Given `meta_base` is used by checkpoint metadata update paths and corruption checks, this needs a strict compatibility contract.

**Needed**: define exact key-removal semantics, per-handle-type key lists, and fallback trigger criteria tied to parse outcomes.

### 2) Ownership and cleanup contracts for the new WS2 helpers are not defined

The proposed split (`_set_from_meta`, `_meta_base_build`) does not describe ownership transfer and cleanup responsibilities for `metaconf`, `base`, and fallback buffers on all error paths.

**Needed**: explicitly document allocation/free ownership and required cleanup for success, fallback, and failure paths.

### 3) Test plan misses failure-injection and concurrency coverage for WS2

The test plan covers functional suites and perf, but does not explicitly include:
- open retry after induced config-set failure,
- concurrent alter/checkpoint/open races while validating reuse/refresh decisions,
- disaggregated stable-URI metadata lookup coverage,
- tiered metadata churn (`last`/`tiers`) with reopen validation.

For this change set, those are key correctness surfaces.

### 4) WS1 fast-path eligibility rules are not deterministic yet

"Fallback on ambiguous input patterns not covered by parity tests" is not an implementable runtime rule. Eligibility/fallback must be based on concrete parser/token conditions, not test corpus scope.

## Minor Issues

### 1) Success criteria should include explicit guardrails for fast-path usage rates

The document mentions optional counters in open questions; given fallback-heavy behavior can erase gains, the plan should include basic hit/miss instrumentation requirements in the success criteria.

## Strengths

- Clear semantic invariants are stated up front for collapse, merge, and dhandle metadata safety.
- Phased rollout with fallback paths shows good risk containment.
- The plan correctly targets known parser-heavy functions and separates P0/P1/P2 by expected ROI.
- The test plan includes parity and performance validation intent, not only functional regressions.

## Recommendations

1. **Lock WS2 reuse semantics now**: mandate strict full metadata-string equality and reject normalized-base equality in this phase.
2. **Specify the metadata-fetch helper contract**: include mandatory URI normalization, reuse preconditions (`cfg[1]` validity), and exact error mapping behavior.
3. **Define the dedicated base-builder compatibility contract**: per-handle-type strip sets, edge-case handling, and when to fall back to legacy.
4. **Expand WS2 tests**: add failure-injection and concurrency scenarios, plus explicit disaggregated/tiered reopen tests.
5. **Make WS1 fallback deterministic**: define token/state-based eligibility and capture fast-path hit/miss metrics.

## Review Checklist

- [x] Read all referenced source files, not just the document
- [x] Verified thread safety claims against actual lock patterns
- [x] Traced memory ownership for all allocations
- [x] Checked all error paths for proper cleanup
- [x] Confirmed API consistency with existing codebase patterns
- [x] Identified undefined/missing proposed function implementations
- [x] Verified type/semantic consistency for key invariants
- [x] Assessed proposed test coverage adequacy
- [x] Checked naming and behavior consistency risks
