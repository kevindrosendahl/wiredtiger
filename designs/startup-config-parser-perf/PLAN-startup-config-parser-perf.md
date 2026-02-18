# Startup Config Parser Performance Plan

## Summary

This plan targets cold-start `ready_wait` CPU spent in WiredTiger config parsing for mongod/mongolite startup paths.

The current branch already includes:

- `wiredtiger_open_ex` structured open config and fallback integration.
- Btree config caching on `WT_DATA_HANDLE`.

Profiling still points at parser-heavy startup paths in:

- `src/config/config_collapse.c` (`__wt_config_collapse`, `__wt_config_merge`, `__config_merge_scan`)
- `src/schema/schema_create.c` (table/colgroup/index/file create flows)
- `src/conn/conn_dhandle.c` (`__wt_conn_dhandle_open` config setup)

The goal is to reduce parser rescans without changing semantics.

### Review Incorporation (2026-02-17)

This revision incorporates review feedback and locks the following design decisions:

- WS2 metadata lookup must always use existing dhandle-name normalization semantics.
- WS2 reuse gating requires a fully initialized cached config (`dhandle->cfg[1]` present).
- WS2 reuse gating uses strict full metadata-string equality only.
- WS2 base derivation must preserve current canonicalization-before-strip behavior.
- WS1 fast-path eligibility/fallback is defined by deterministic parser/token conditions.
- WS1 key identity/resolution behavior must match legacy token semantics; unsupported dotted-key traversal cases must fall back.
- WS1 projected-base duplicate-key behavior must match legacy collapse behavior.
- WS2 helper ownership and cleanup responsibilities are explicit.
- WS2 testing now includes deterministic failure-injection and concurrency/race scenarios.
- WS1/WS2 observability now defines counter scope/lifecycle and acceptance thresholds.

## Problem Statement

Two patterns still dominate startup parsing:

1. **Repeated collapse/merge rescans in schema create/open**
   - `__wt_config_collapse` walks base keys and repeatedly calls `__wti_config_get`, which rescans config strings.
   - `__wt_config_merge` recursively scans, flattens, sorts, and formats for each call site.
   - `schema_create.c` invokes collapse/merge repeatedly for closely related stacks.

2. **Per-open dhandle config rebuild**
   - `__wt_conn_dhandle_open` always clears and rebuilds dhandle config.
   - `__conn_dhandle_config_set` does a `collapse` plus `merge(strip)` sequence for metadata base derivation.
   - This repeats parser work even when effective metadata inputs are unchanged.

## Goals

- Materially reduce `__config_next` / `__config_merge_scan` CPU in startup slices.
- Preserve exact behavior and on-disk metadata correctness.
- Keep changes scoped to startup-heavy call paths first.

## Non-Goals

- No user-visible config syntax changes.
- No broad parser rewrite across all WiredTiger APIs in first phase.
- No removal of legacy merge/collapse code paths.

## Semantic Invariants (Must Hold)

### Collapse semantics (`__wt_config_collapse`)

- Result key set is projected from the first config string.
- If the first config string repeats a key, output preserves one entry per occurrence (no dedup of projected keys).
- For each projected key, the final value is the last value across config stack.
- Nested structures are not field-merged for collapse; top-level value replacement semantics remain.
- Quoting preservation for string keys/values remains unchanged.

### Merge semantics (`__wt_config_merge`)

- Last-wins precedence across config stack.
- Nested structures are recursively merged with current behavior.
- Strip semantics remain identical (including nested removals used by existing strip strings).
- Output remains valid config syntax for all legacy inputs.

### Dhandle metadata/base semantics

- `dhandle->meta_base`, `meta_hash`, `orig_meta_base`, and checkpoint update safety checks remain valid.
- Existing correctness checks in `meta_ckpt.c` continue to work unchanged.
- Reconfigure/alter/drop/checkpoint/special-handle flows remain correct.

## Proposed Workstreams

## WS1: Fast Collapse Path For Startup-Heavy Callers (P0)

### Scope

- `src/config/config_collapse.c`
- `src/schema/schema_create.c` call sites that currently use collapse repeatedly

### Design

Add an internal optimized path for collapse that avoids repeated full rescans.

Proposed internal helper:

- `__wti_config_collapse_fast(WT_SESSION_IMPL *session, const char **cfg, char **config_ret, bool *used_fast_path)`

Behavior:

- Attempt fast path first; if unsupported/unsafe, set `used_fast_path=false` and fall back to existing `__wt_config_collapse`.
- Keep legacy implementation as source of truth for fallback and validation.

### Fast-path algorithm (top-level)

1. Parse base config (`cfg[0]`) once and record projected keys in order, preserving duplicate occurrences.
2. Parse each config string once and build last-wins lookup for top-level keys.
3. Emit output by iterating projected base keys (including duplicates) and resolving each occurrence from the lookup.
4. Preserve quotes for string keys/values exactly as legacy path does.

### WS1 key identity parity rules

- Key equality in fast path matches legacy parser behavior: token-byte identity (`len` + bytewise compare), with no case folding or normalization.
- Quoted/unquoted forms are treated according to parser token output; fast path must not add extra normalization beyond parser semantics.
- If a projected base key requires dotted traversal semantics (legacy `__config_getraw` subkey behavior), fast path must fall back unless dotted traversal is explicitly implemented with parity tests.

### Fast-path fallback conditions

Fast-path eligibility is deterministic. Use fast path only when all of the following are true:

- Base config (`cfg[0]`) parses successfully using `__wt_config_next`.
- Every parsed base key token type is `WT_CONFIG_ITEM_STRING` or `WT_CONFIG_ITEM_ID`.
- Every additional config string parses successfully at top-level with `__wt_config_next`.
- All key tokens in additional strings are `WT_CONFIG_ITEM_STRING` or `WT_CONFIG_ITEM_ID`.

Fallback to legacy collapse on the first deterministic violation:

- Any parse error from `__wt_config_next`.
- Any key token type outside `STRING/ID`.
- Any internal allocation/formatting error in fast-path structures.

### Schema integration

Use the new fast collapse helper in startup-heavy schema call paths:

- `__create_file`
- `__create_colgroup`
- `__create_index`
- `__create_table`
- other create-path collapse call sites in the same file

Important: this is call-site routing only; semantic behavior stays identical.

## WS2: Dhandle Config Rebuild Reduction (P1)

### Scope

- `src/conn/conn_dhandle.c`

### Design

Split current config setup into two concerns:

1. **Metadata fetch/change detection**
2. **Base metadata derivation**

Proposed refactor:

- `__conn_dhandle_metadata_lookup(WT_SESSION_IMPL *session, char **metaconfp)`
- `__conn_dhandle_config_set_from_meta(WT_SESSION_IMPL *session, const char *metaconf)`
- `__conn_dhandle_meta_base_build(WT_SESSION_IMPL *session, const char *metaconf, char **basep, bool *used_legacy_fallback)`

`__conn_dhandle_metadata_lookup` is the single entry point for metadata fetch in both first-build and reuse-check flows. It must preserve the existing URI normalization behavior used today before `__wt_metadata_search` (including disaggregated/stable URI forms).
It must also preserve existing error mapping semantics (including `WT_NOTFOUND` handling) used by current open paths.
The normalized lookup name is local to the helper (scratch-backed) and is not returned.

### Open-path reuse rule

In `__wt_conn_dhandle_open`:

- If a previously built `dhandle->cfg` exists, fetch latest metadata once via normalized lookup.
- Reuse is allowed only if `dhandle->cfg != NULL && dhandle->cfg[1] != NULL`.
- If the above precondition fails, treat cached config as invalid and force clear/rebuild.
- If precondition holds, compare latest metadata with cached metadata using strict full metadata-string equality.
- Only if strings are exactly equal may setup skip clear/rebuild.
- If metadata changed, rebuild config and clear btree conf cache.

This retains correctness while avoiding repeated clear/reparse when inputs are stable.

### Reuse equality decision (locked)

For this phase, reuse gating is based on strict full metadata-string equality only.

Normalized-base equality is explicitly out of scope because tiered open logic depends on fields in full metadata (for example, `last` and `tiers`) that are intentionally absent from normalized base in current logic.

### Base-build optimization

For rebuild path, avoid the current `collapse + merge(strip)` chain by using a dedicated metadata-base builder that:

- applies the same two-step transform used by current code, in the same order:
  1. Canonicalization overwrite (collapse-equivalent) with:
     - `checkpoint=()`
     - `checkpoint_backup_info=()`
     - `live_restore=`
  2. Type-specific strip pass (merge-strip equivalent):
     - btree: `checkpoint`, `checkpoint_backup_info`, `checkpoint_lsn`, `live_restore`
     - tiered: `checkpoint`, `checkpoint_backup_info`, `checkpoint_lsn`, `flush_time`, `flush_timestamp`, `last`, `tiers`
- applies canonicalization using collapse projection semantics with `metaconf` as the projected key set (overwrite updates existing top-level keys only; no introduction of absent keys),
- treats key matching as exact top-level key matching (not prefix/substring matching),
- applies stripping by key name regardless of value form (scalar, quoted string, or struct/list payload),
- preserves all non-stripped keys and their parsed values with equivalent formatting semantics,
- preserves compatibility with `meta_base` hashing/corruption-check usage,
- targets byte-for-byte equivalence with current output for the same input metadata and dhandle type.

Fallback to legacy base derivation must occur on deterministic conditions:

- parse/tokenization errors while building base,
- unsupported token/key forms for the dedicated builder,
- allocation/formatting failures,
- any handle type lacking an explicit strip contract,
- any internal equivalence check failure in diagnostic mode.

Legacy path remains available as fallback until parity confidence is complete.

### Ownership and cleanup contract (WS2)

Helper ownership rules:

- `__conn_dhandle_metadata_lookup` returns owned `metaconf` memory to caller on success.
- `__conn_dhandle_metadata_lookup` owns and frees any temporary normalized-name scratch before return (no lookup-name out-param ownership).
- `__conn_dhandle_meta_base_build` returns owned `base` memory to caller on success.
- `__conn_dhandle_config_set_from_meta` transfers ownership into `dhandle->cfg[1]` (`metaconf`) and `dhandle->meta_base` (`base`) only after full success.

Failure-path rules:

- If setup fails before ownership transfer, caller frees local `metaconf`/`base`.
- If any partial `dhandle->cfg` allocation exists without `dhandle->cfg[1]`, the next open treats it as invalid and forces clear/rebuild.
- Reuse path never dereferences cached metadata unless `dhandle->cfg[1]` is non-NULL.
- On reuse-hit equality (skip rebuild), the newly fetched `metaconf` from lookup is freed immediately because ownership is not transferred.

## WS3: Optional Open-Config Coverage Expansion (P2, lower ROI)

This branch already has open struct config support, but many `conn_api.c` reads still go through string lookups.
After P0/P1, optionally widen struct-aware getter usage in connection open/reconfig paths where practical.

This is intentionally deferred because startup schema/dhandle parsing is expected to provide larger wins.

## Detailed Test Plan

## 1) Unit parity tests for collapse/merge behavior

Add/extend Catch2 config tests (likely in `test/catch2/misc_tests`):

- Nested structs with partial overrides.
- Duplicate key precedence across multi-string stacks.
- Duplicate projected keys in `cfg[0]` are preserved one-for-one (no accidental dedup).
- Strip-equivalent cases used in current code.
- Quoted keys and quoted string values.
- Empty values and bool shorthand forms.
- Legacy-vs-fast output equivalence assertions.

## 2) Schema integration coverage

Run existing schema suites emphasizing create/open paths:

- `test/suite/test_schema01.py`
- `test/suite/test_schema02.py`
- `test/suite/test_schema03.py`
- relevant import/tiered schema tests already in suite

## 3) Dhandle/checkpoint safety coverage

Exercise checkpoint and metadata-update-sensitive paths:

- existing checkpoint csuite tests
- btree config cache csuite tests under `test/csuite/wt_open_conf`
- open retry after induced config-setup failure (simulate failure between `dhandle->cfg` allocation and metadata assignment)
  - add deterministic diagnostic failpoint in `conn_dhandle` on this exact edge, then assert retry correctness
- disaggregated stable-URI metadata lookup validation (normalized-name lookup correctness)
- tiered metadata churn reopen validation (`last`/`tiers` changes must force rebuild)
- concurrent/repeated open while alter/checkpoint metadata updates are occurring (reuse/refresh race coverage)

## 4) Performance validation

Microbench:

- repeated schema create/open operations with representative config stacks.
- repeated dhandle open cycles with unchanged metadata.

End-to-end:

- mongolite/mongod cold-start ready_wait runs with same harness/settings.
- confirm reduced hotspot share in `__config_merge_scan` and `__config_next`.

Methodology:

- Run at least 15 cold-start trials per variant under the same harness/settings.
- Compare medians across variants; also record p95 to detect instability/noise.
- Apply acceptance thresholds to the 15-run median results; investigate any p95 regressions larger than 5% even when medians pass.

## Observability Guardrails

Add lightweight counters for startup-focused validation:

- WS1: fast collapse attempts / hits / fallbacks
- WS2: dhandle reuse-check attempts / reuse-hits / forced-rebuilds

Counter scope/lifecycle:

- Use connection-level statistics counters for rollout so values are visible in standard stats collection.
- Reset semantics follow normal connection statistics lifecycle (new connection starts at zero; clear/reset follows existing statistics behavior).
- Keep counters as permanent stats through rollout.
- Retention decision trigger: after three consecutive benchmark cycles where thresholds are met and hit rates stay above 70% in target workloads, explicitly decide keep/remove in a follow-up design update.

Guardrail for completion: in targeted startup benchmarks, hit rates must be high enough that fast paths are not fallback-dominated. If fallback dominates, optimization is not considered complete.

Initial acceptance thresholds:

- Combined hotspot share (`__config_next` + `__config_merge_scan`) in startup-ready_wait slices decreases by at least 20%.
- End-to-end ready_wait median improves by at least 3% under the existing benchmark harness/settings.
- WS1 and WS2 fast-path hit rates each exceed 70% on the target startup workload.

## Rollout Strategy

Phase A:

- land WS1 fast collapse helper plus call-site routing.
- keep feature behind internal runtime switch if needed during shakeout.

Phase B:

- land WS2 dhandle reuse + base-build optimization.
- preserve fallback path for error/parity guardrails.

Phase C (optional):

- evaluate WS3 additional struct-config open coverage.

## Risk Assessment

- **Semantic drift risk (high):** mitigated by strict parity tests and fallback to legacy path.
- **Checkpoint metadata safety risk (high):** mitigated by preserving `meta_base` invariants and reusing existing validation checks.
- **Edge-case parser behavior risk (medium):** mitigated by narrow fast-path eligibility + fallback.
- **Limited perf gain risk (medium):** mitigated by measuring at each phase before expanding scope.

## Success Criteria

- No functional regressions in WiredTiger test suites.
- Startup profiler shows material reduction in parser hotspots (`__config_next`, `__config_merge_scan`).
- Ready-wait cold-start time improves in mongolite/mongod benchmark runs.
- WS1/WS2 observability counters show fast paths are meaningfully exercised in startup benchmarks.
- Initial acceptance thresholds in "Observability Guardrails" are met.

## Open Questions

- Whether WS1 should remain scoped to startup-heavy call sites long-term or be promoted to broader collapse users after parity confidence.
