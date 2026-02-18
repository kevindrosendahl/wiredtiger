# Review: `PLAN-startup-config-parser-perf.md`

## Critical Issues

1. **WS2 base-build spec does not fully preserve current canonicalization semantics before stripping.**
   - Current `__conn_dhandle_config_set` behavior for `WT_DHANDLE_TYPE_BTREE` and `WT_DHANDLE_TYPE_TIERED` is a two-step transform: first `__wt_config_collapse` with overwrite entries (`checkpoint=()`, `checkpoint_backup_info=()`, `live_restore=`), then `__wt_config_merge(..., strip=...)`.
   - The design's WS2 base builder describes only top-level key stripping. For tiered handles, the strip contract explicitly omits `live_restore`, while current code still canonicalizes `live_restore` to empty *before* stripping.
   - If WS2 implementation follows the current written spec literally, tiered `meta_base` can retain non-canonical `live_restore` payloads (for example bitmap/nbits content) that are currently normalized away. That changes `meta_base`/`meta_hash` behavior and risks divergence in checkpoint metadata update paths that rely on base-string stability.
   - **Required fix in design:** specify the exact canonicalization transform to match today's collapse+merge behavior byte-for-byte (including pre-strip overwrite semantics), not only the final strip key list.

## Medium Issues

1. **`__conn_dhandle_metadata_lookup` ownership/lifetime for `lookup_namep` is under-specified.**
   - The proposed helper returns `lookup_namep` and `metaconfp`, but ownership rules only document `metaconf`/`base`.
   - Existing normalization uses a temporary scratch buffer via `__wt_btree_shared_base_name(...)` and frees it in the same function scope. Returning a normalized name without explicit ownership can create leaks or dangling references.
   - **Needed:** explicit contract for `lookup_namep` lifetime (borrowed from `dhandle->name`, owned allocation, or no out-param at all).

2. **WS1 fast-path spec does not explicitly preserve duplicate projected keys from `cfg[0]`.**
   - Existing `__wt_config_collapse` iterates `cfg[0]` sequentially and emits one output entry per base occurrence, even if keys repeat.
   - The fast-path description ("record projected keys in order" + lookup map) can be implemented either preserving duplicates or deduplicating unintentionally.
   - **Needed:** explicit statement that duplicates in `cfg[0]` must be emitted with legacy-equivalent behavior.

3. **Failure-injection test requirement is good, but currently not operationally defined.**
   - The plan asks for "simulate failure between `dhandle->cfg` allocation and metadata assignment" but does not identify an existing deterministic failpoint/hook in this path.
   - Without a concrete mechanism, this test is not reliably implementable in CI.
   - **Needed:** add a specific diagnostic failpoint (or equivalent deterministic injection mechanism) in `conn_dhandle` and reference it in test plan.

## Minor Issues

1. **Observability counters are under-scoped.**
   - WS1/WS2 counters are listed, but placement and lifecycle are not (connection stats vs dhandle stats, reset semantics, permanence).
   - This affects rollout interpretation and long-term maintenance cost.

2. **Performance validation section should define acceptance thresholds.**
   - "Material reduction" is directionally correct but not quantified. A minimum expected delta would improve go/no-go clarity.

## Strengths

- The document clearly separates semantic invariants from optimization mechanics.
- WS2 correctly calls out normalized metadata lookup requirements (`__wt_btree_shared_base_name`-equivalent behavior) and `WT_NOTFOUND` mapping constraints.
- Reuse gating requirement (`dhandle->cfg[1] != NULL`) directly addresses partial-initialization safety.
- Test plan includes parity, integration, checkpoint-safety, and concurrency dimensions instead of only happy-path throughput.

## Recommendations (Priority Order)

1. Update WS2 base-builder spec to codify full legacy-equivalent transform (pre-strip overwrite + strip), not just strip keys.
2. Add explicit ownership/lifetime rules for every WS2 helper out-parameter, including normalized lookup names.
3. Add a deterministic failure-injection hook in `conn_dhandle` and wire it into csuite coverage.
4. Add WS1 parity tests for duplicate base keys and quoted-key edge cases to prevent accidental dedup behavior.
5. Define counter scope/reset semantics and concrete perf acceptance thresholds before implementation starts.

## Review Checklist

- [x] Read all referenced source files, not just the document
- [x] Verified thread safety claims against actual lock patterns
- [x] Traced memory ownership for all allocations in proposed touched paths
- [x] Checked all error-path expectations for cleanup semantics
- [x] Confirmed API consistency expectations with current code patterns
- [x] Identified under-defined helper/function contracts
- [x] Verified type/semantic consistency across proposed WS1/WS2 behavior
- [x] Assessed test coverage adequacy and implementation feasibility
- [x] Checked naming/contract consistency throughout the design
