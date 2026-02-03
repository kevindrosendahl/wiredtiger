# Design Review: Local Mode Step-Down Checkpoint

**Document Reviewed**: `PLAN-local-mode-stepdown-checkpoint.md`  
**Review Date**: 2026-02-03 17:52  
**Reviewer**: Independent Design Review (Fresh Session)

---

## Summary

This is a well-revised design document that has addressed the critical issues from prior reviews. The core approach of performing checkpoint BEFORE acquiring the checkpoint lock is correct and avoids the lock flag desynchronization issue. The document demonstrates good understanding of WiredTiger internals and follows established patterns.

---

## Critical Issues

**None identified.** Prior critical issues have been resolved:

1. ~~NULL config to `__wt_checkpoint_db`~~ - **Fixed**: Now uses proper config array with `WT_CONFIG_BASE(session, WT_SESSION_checkpoint)` and `"force=true"`
2. ~~Lock flag desynchronization~~ - **Fixed**: Checkpoint now performed before `WT_WITH_CHECKPOINT_LOCK` call
3. ~~Non-existent `__layered_config_get_bool`~~ - **Fixed**: Now correctly uses `__layered_config_get_int` pattern

---

## Medium Issues

### 1. Configuration Parsing Location May Conflict With Connection Init

**Location**: Implementation Details, Configuration Parsing section (proposed line ~1989)

The design proposes adding `local_mode` parsing in the "Common settings" section BEFORE role handling. However, examining the actual code:

```c
// Line 1989-1990 in conn_layered.c
/* Common settings between initial connection config and reconfig. */

/* Get the last materialized LSN. */
```

The proposed insertion point is correct. However, the design shows parsing `local_mode` and calling `F_SET(&conn->disaggregated_storage, WT_DISAGG_LOCAL_MODE)`, but this flag is used during step-down which only occurs during **reconfig**, not initial connection.

**Concern**: Setting the flag during initial connection open is harmless but unnecessary. The flag only affects behavior in the step-down path which requires `reconfig=true`.

**Recommendation**: This is acceptable as-is, but consider adding a comment clarifying that the flag is only meaningful during reconfigure operations.

### 2. Test Pattern Differs From Existing Tests

**Location**: Testing Strategy section

The proposed test `test_local_mode_stepdown_visibility` uses this pattern:

```c
testutil_check(wiredtiger_open(home, NULL,
  "create,statistics=(all),"
  "extensions=[" WT_BUILDDIR "/ext/page_log/palite/libwiredtiger_palite.so],"
  "disaggregated=(role=\"leader\",page_log=palite,local_mode=true)", &conn1));
```

**Issue**: The existing `test_palite_multiprocess/main.c` uses a different pattern for extension loading:

```c
testutil_snprintf(config, sizeof(config),
  "create,statistics=(all),"
  "extensions=[%s/ext/page_log/palite/libwiredtiger_palite.so="
  "(config=\"(synchronous=2)\")],"
  "disaggregated=(role=\"leader\",page_log=palite)",
  WT_BUILDDIR);
```

The test uses `testutil_snprintf` with `%s` for `WT_BUILDDIR`, while the proposed test uses string concatenation with `WT_BUILDDIR` directly. Both work, but for consistency with the existing test file, use the `testutil_snprintf` pattern.

**Recommendation**: Use `testutil_snprintf` pattern to match existing tests in the file.

### 3. Follower Opening With `local_mode=true`

**Location**: Testing Strategy, `test_local_mode_stepdown_visibility`

The test opens the follower connection with `local_mode=true`:

```c
testutil_check(wiredtiger_open(home, NULL,
  "statistics=(all),"
  "...disaggregated=(role=\"follower\",page_log=palite,local_mode=true)", &conn2));
```

**Question**: Is `local_mode=true` meaningful for a follower? The flag only affects the step-down path (leader → follower). Opening as follower directly never triggers that code path.

**Recommendation**: Either:
1. Remove `local_mode=true` from follower configs in tests (since it's meaningless)
2. Or add a comment explaining it's included for symmetry/configuration completeness

### 4. Missing Test for Step-Down WITHOUT `local_mode`

**Location**: Testing Strategy section

The tests cover `local_mode=true` behavior but don't verify that `local_mode=false` (or default) preserves the original behavior (no checkpoint on step-down).

**Recommendation**: Add a test verifying that step-down without `local_mode` does NOT trigger an extra checkpoint. This confirms backward compatibility.

---

## Minor Issues

### 5. Flag Documentation Comment Inconsistency

**Location**: Data Structures section

The design shows:

```c
#define WT_DISAGG_LOCAL_MODE 0x2u  /* NEW: Enable local mode checkpoint on step-down */
```

But the design's description says "Enable local mode semantics". The comment should match:

```c
#define WT_DISAGG_LOCAL_MODE 0x2u  /* Enable local mode semantics for step-down */
```

### 6. Error Message Inconsistency

**Location**: Implementation Details, modified `__wti_disagg_conn_config`

The design shows two error messages:

```c
__wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
  "Local mode step-down checkpoint failed: %s", wiredtiger_strerror(ret));
WT_ERR_MSG_CHK(session, ret, "Failed to checkpoint during local mode step-down");
```

The verbose message includes the error string (`wiredtiger_strerror(ret)`), but `WT_ERR_MSG_CHK` will also produce an error message. This results in two similar but slightly different error outputs for the same failure.

**Recommendation**: Remove the verbose message before `WT_ERR_MSG_CHK` since the macro already logs the error, or change the verbose message to use a different verbosity level (e.g., `__wt_verbose_debug2`) for detailed tracing.

### 7. Test `test_local_mode_stepdown_idempotent` Missing Table Creation

**Location**: Testing Strategy, `test_local_mode_stepdown_idempotent`

The test calls reconfigure multiple times but never creates any tables:

```c
testutil_check(wiredtiger_open(home, NULL,
  "create,statistics=(all),...", &conn));
/* First step down */
testutil_check(conn->reconfigure(conn, "disaggregated=(role=\"follower\")"));
```

Without any tables, the checkpoint during step-down will be trivial. While this tests idempotency, it doesn't exercise the actual checkpoint path with data.

**Recommendation**: Consider adding table creation and some data to make the idempotency test more realistic.

### 8. `disagg_step_down_time` Statistic Documentation

**Location**: Implementation Details

The design notes:

> When `local_mode=true`, this statistic will include the checkpoint duration.

This is a behavior change from the current semantics where `disagg_step_down_time` only measures the step-down itself. Users monitoring this statistic may see unexpected increases.

**Recommendation**: Add a note in Phase 4 documentation about this statistic change, or consider adding a separate statistic for the local-mode checkpoint duration.

---

## Verification Against Source Code

### Verified Claims

| Claim | Verified | Notes |
|-------|----------|-------|
| `__disagg_step_down` returns void | ✓ | Line 1925 in conn_layered.c |
| `__disagg_step_down` asserts checkpoint lock owned | ✓ | Line 1932 |
| `WT_WITH_CHECKPOINT_LOCK` macro re-entrancy | ✓ | Lines 142-153 in schema.h |
| `__wt_checkpoint_db` acquires checkpoint lock internally | ✓ | Line 1816 in checkpoint_txn.c |
| `WT_DISAGG_NO_SYNC` flag exists in struct | ✓ | Line 240 in connection.h |
| `__layered_config_get_int` function exists | ✓ | Line 17 in conn_layered.c |
| Config array pattern with `WT_CONFIG_BASE` | ✓ | Multiple callers verified |
| "Common settings" section at line 1989 | ✓ | Actual code matches |
| Step-down occurs at lines 2033-2042 | ✓ | Actual code matches |

### Flag Value Assignment

The design proposes `WT_DISAGG_LOCAL_MODE 0x2u`. Current flags in `WT_DISAGGREGATED_STORAGE`:

```c
#define WT_DISAGG_NO_SYNC 0x1u
```

The value `0x2u` is the correct next value and will be auto-generated correctly when using the `AUTOMATIC FLAG VALUE GENERATION` markers.

---

## Strengths

1. **Thorough Prior Review Integration**: The document has been updated to address all critical issues from previous reviews.

2. **Correct Lock Handling**: The checkpoint-before-lock approach correctly avoids the flag/spinlock desynchronization issue.

3. **Appropriate Flag Location**: Using `WT_DISAGGREGATED_STORAGE.flags` instead of connection flags avoids auto-generation conflicts.

4. **Comprehensive Race Window Documentation**: The document clearly explains the visibility guarantee and caller responsibility.

5. **Good Transaction Handling Documentation**: The in-flight transaction behavior is now documented with appropriate references to `__checkpoint_prepare`.

6. **Backward Compatible Design**: Default `local_mode=false` ensures no behavior change for existing users.

7. **`force=true` Usage**: Correctly ensures checkpoint happens even on clean data.

---

## Recommendations

### Before Implementation

1. **Align test patterns** with existing `test_palite_multiprocess/main.c` (use `testutil_snprintf` for config strings)

2. **Add backward-compatibility test** verifying `local_mode=false` doesn't trigger checkpoint on step-down

3. **Clarify follower config** - either remove `local_mode=true` from follower configs or document why it's there

### During Implementation

4. **Run `python dist/s_all.py`** after adding to `api_data.py` to verify correct key ID generation

5. **Verify flag value** after regeneration matches expected `0x2u`

### After Implementation

6. **Document statistic behavior change** for `disagg_step_down_time` in documentation

---

## Verification Checklist

- [x] Read all referenced source files
- [x] Verified thread safety claims against actual lock patterns
- [x] Traced memory ownership for all allocations (stack-only, verified)
- [x] Checked all error paths for proper cleanup (WT_ERR pattern correct)
- [x] Confirmed API consistency with existing codebase
- [x] Identified all undefined function implementations (none - uses existing)
- [x] Verified type consistency throughout
- [x] Assessed test coverage adequacy

---

## Conclusion

**Ready for implementation with minor adjustments.** The design is sound and addresses all previously identified critical issues. The remaining issues are minor consistency and documentation items that can be resolved during implementation.

The core approach is correct: checkpoint before acquiring the step-down lock, using proper config arrays, and storing the flag in the appropriate struct. The backward-compatible opt-in design ensures no risk to existing users.
