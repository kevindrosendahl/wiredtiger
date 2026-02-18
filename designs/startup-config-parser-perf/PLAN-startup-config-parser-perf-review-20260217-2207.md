# Review: `PLAN-startup-config-parser-perf.md`

## Critical Issues

None identified in this revision.

## Medium Issues

1. **WS2 canonicalization still needs an explicit projection contract.**
   - Current behavior in `src/conn/conn_dhandle.c` builds `meta_base` by first calling `__wt_config_collapse` (with `metaconf` as `cfg[0]`) and then stripping via `__wt_config_merge`.
   - In `src/config/config_collapse.c`, collapse projects keys from the first string. That means overwrite entries do not introduce new keys that were absent in `metaconf`.
   - The current WS2 text says "collapse-equivalent", but it should state this projection rule explicitly. If a dedicated builder introduces absent keys (especially `live_restore` for tiered handles, which is not in the tiered strip list), `meta_base` / `meta_hash` behavior can drift and affect checkpoint update behavior in `src/meta/meta_ckpt.c`.
   - **Required:** define canonicalization as overwrite-only-on-existing-top-level-keys (no key introduction), then strip.

2. **WS1 key-resolution parity with legacy lookup remains under-specified.**
   - Legacy lookup (`__wti_config_get` / `__config_getraw` in `src/config/config.c`) matches keys by token text (`len` + `memcmp`) and supports dotted subkey traversal.
   - The WS1 fast path is described as top-level parse + lookup table. Without an explicit rule, implementations can diverge on keys containing `.` or mixed quoted/unquoted key forms.
   - **Required:** explicitly define key identity to match legacy token behavior, and require fallback for dotted-base-key cases unless dotted traversal is implemented in fast path.

3. **WS2 ownership contract should explicitly cover reuse-hit cleanup.**
   - `__conn_dhandle_metadata_lookup` returns owned `metaconf`.
   - On equality hit (`latest metadata == dhandle->cfg[1]`), the newly fetched `metaconf` must be freed because ownership is not transferred.
   - This is implied by current ownership language, but adding one explicit bullet avoids ambiguity in a high-risk path.

## Minor Issues

1. **Perf thresholds are defined, but run methodology is not.**
   - The document gives concrete targets, which is good.
   - It does not define run count/noise handling (for example, median of N cold starts), so pass/fail can vary across runs.

2. **Long-term counter retention criteria are still open-ended.**
   - Counter scope/lifecycle is well-specified.
   - "Reassess after stabilization" should include an explicit trigger/decision condition.

## Strengths

- Prior critical issues from earlier reviews are now addressed in the document (normalized lookup semantics, strict full-metadata equality, `cfg[1]` reuse gating, canonicalization-before-strip ordering).
- WS2 ownership/failure-path intent is much clearer and directly targets partial-initialization risk in `conn_dhandle`.
- Test coverage now includes deterministic failure-injection and concurrency/race scenarios in addition to parity and perf checks.
- Observability section now includes concrete thresholds and counter lifecycle intent.

## Recommendations

1. Add explicit "no key introduction" language to WS2 canonicalization semantics.
2. Add explicit WS1 key-equivalence and dotted-key fallback rules matching `__wti_config_get`.
3. Add one explicit reuse-hit cleanup bullet in WS2 ownership section.
4. Add benchmark methodology (run count + statistic) to make perf gates reproducible.

## Review Checklist

- [x] Read all referenced source files, not just the document
- [x] Verified thread safety claims against actual lock patterns
- [x] Traced memory ownership for all allocations
- [x] Checked all error paths for proper cleanup
- [x] Confirmed API consistency with existing codebase patterns
- [x] Identified all undefined/missing function implementations
- [x] Verified type consistency throughout
- [x] Assessed test coverage adequacy
- [x] Checked for naming consistency
