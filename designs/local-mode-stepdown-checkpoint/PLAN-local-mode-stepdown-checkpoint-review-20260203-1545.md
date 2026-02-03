# Design Review: Local Mode Step-Down Checkpoint

**Reviewer**: AI Design Reviewer (Fresh Session)  
**Date**: 2026-02-03 15:45  
**Document**: `PLAN-local-mode-stepdown-checkpoint.md`

## Executive Summary

The design proposes adding a checkpoint before step-down in local disaggregated mode to ensure data visibility for followers. The core technical analysis is sound: the lock handling rationale is correct, and the placement of checkpoint in the caller is the right approach. However, there are issues with redundant state management, error handling completeness, and test reliability that should be addressed.

---

## Critical Issues

### 1. Redundant State with `WT_CONN_DISAGG_LOCAL_MODE` Flag

**Problem**: The design introduces a new connection flag `WT_CONN_DISAGG_LOCAL_MODE` to track local mode, but this creates redundant state that can drift out of sync with the actual configuration.

**Source Verification**: The existing `__wt_conn_is_disagg()` function (conn_layered.c:2172-2181) already checks if disaggregated storage is active:

```c
bool
__wt_conn_is_disagg(WT_SESSION_IMPL *session)
{
    // ...
    return (disagg->page_log_meta != NULL);
}
```

The `local_mode` configuration value should be stored in a more appropriate location rather than as a connection-wide flag that duplicates state.

**Recommendation**: Either:
- Store `local_mode` as a boolean field in `WT_DISAGGREGATED_STORAGE` struct (connection.h:191-243), or
- Query the config value directly when needed rather than caching it

If keeping the flag, add an assertion that verifies `WT_CONN_DISAGG_LOCAL_MODE` is only set when `__wt_conn_is_disagg()` returns true.

### 2. Configuration Parsing Order May Cause Issues

**Problem**: The proposed configuration parsing happens before role initialization, but references the `local_mode` flag during step-down which occurs later. The order in `__wti_disagg_conn_config` is:

1. Parse `last_materialized_lsn` (line ~1996)
2. Parse role (line ~2010-2018)
3. Initial role set OR step-up/step-down (lines ~2020-2042)
4. [Proposed] Parse `local_mode` after reconfig check

The design shows parsing `local_mode` only when `!reconfig`, but the flag needs to be available during reconfig for step-down.

**Source Verification**: Looking at conn_layered.c lines 2033-2042, the step-down path executes during reconfig when `was_leader && !leader`.

**Recommendation**: Parse `local_mode` before the role handling section (before line 2010), not after the `if (reconfig) goto err;` check. This ensures the flag is set before any step-down logic executes, including during reconfig.

### 3. Error Path May Leave Connection in Inconsistent State

**Problem**: If `__wt_checkpoint_db()` fails, the design shows returning immediately without cleanup. However, the caller (`conn->reconfigure`) may have already modified other state before calling `__wti_disagg_conn_config`.

**Source Verification**: In conn_reconfig.c:479 and 513, `__wti_disagg_conn_config` is called among other reconfigure operations. A failure partway through could leave the connection partially reconfigured.

**Recommendation**: Document the atomicity guarantees (or lack thereof) for the reconfigure operation. Consider whether any rollback is needed if checkpoint fails, or explicitly document that failing to step down leaves the connection as a leader (which appears to be the intent).

---

## Medium Issues

### 4. Test `local_mode_stepdown_checkpoints` Has Race/Reliability Issues

**Problem**: The test captures `WT_STAT_CONN_txn_checkpoint` before and after step-down to verify checkpoint occurred. However:

1. The "before" count is captured after `wiredtiger_open`, which may include checkpoints from connection initialization
2. Other background threads (checkpoint server, etc.) could trigger checkpoints
3. The stat increment timing vs. the checkpoint completion timing could cause flaky tests

**Source Verification**: Looking at the test patterns in test_palite_multiprocess/main.c, the existing tests don't rely on checkpoint counts - they verify data visibility directly.

**Recommendation**: Use a more reliable test approach:
- Add a verbose message specifically for local mode checkpoint and check logs, OR
- Use `session->checkpoint()` immediately before step-down in non-local mode and verify the count is the same as local mode step-down, OR  
- Add a dedicated statistic for local-mode-step-down-checkpoints

### 5. Test Shares `uri` Variable But Doesn't Declare It

**Problem**: The test functions reference `uri` but the variable declaration is not shown in the test code snippets. Looking at the actual test file:

```c
static const char *uri = "table:test";
```

This is a static file-scope variable. The new tests should follow this pattern.

**Recommendation**: Clarify that the tests use the existing static `uri` variable, or define test-specific URIs to avoid any table name conflicts between test runs.

### 6. `force=true` Checkpoint Semantics Should Be Verified

**Problem**: The design states `force=true` ensures checkpoint even with no dirty data, but doesn't verify this claim against WiredTiger checkpoint semantics.

**Source Verification**: In checkpoint_txn.c, the `force` config controls `F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE)` behavior and forces checkpoint naming, but the exact behavior should be confirmed.

**Recommendation**: Add a unit test that explicitly verifies `force=true` creates a checkpoint when no data is dirty (the proposed `local_mode_stepdown_clean_data` test in "Additional Test Scenarios" section addresses this).

### 7. Step-Down Timing Statistic Includes Checkpoint Time

**Problem**: The existing code captures step-down time:
```c
time_start = __wt_clock(session);
// [checkpoint here in local mode]
WT_WITH_CHECKPOINT_LOCK(session, __disagg_step_down(session));
time_stop = __wt_clock(session);
WT_STAT_CONN_SET(session, disagg_step_down_time, ...);
```

With the proposed change, `disagg_step_down_time` will now include checkpoint duration in local mode, making the stat harder to interpret and compare.

**Recommendation**: Either:
- Add a separate stat for local-mode checkpoint duration during step-down, or
- Document that `disagg_step_down_time` includes checkpoint time when `local_mode=true`

---

## Minor Issues

### 8. Verbose Message Placement

**Problem**: The verbose messages use `__wt_verbose_debug2` for checkpoint status but `__wt_verbose_debug1` for other messages. This inconsistency may affect log readability.

**Recommendation**: Use consistent verbosity levels - `debug1` for major state changes, `debug2` for detailed progress.

### 9. Test Home Directory Naming

**Problem**: Test function `test_local_mode_stepdown_visibility` uses `"WT_TEST_visibility"` but other tests in the file use patterns like `"WT_TEST_sync%d"`. The naming should be consistent.

**Recommendation**: Use pattern like `"WT_TEST_local_mode_visibility"` to be consistent with the feature name.

### 10. Config String Documentation

**Problem**: The example config string shows:
```
disaggregated=(role="leader",page_log=palite,local_mode=true)
```

But `page_log` is typically specified separately from `role` and other options in the existing tests.

**Recommendation**: Verify the config string syntax works correctly with the config parser, or update the example to match the actual syntax pattern used in tests.

---

## Strengths

1. **Correct Lock Analysis**: The thread safety analysis of `WT_WITH_CHECKPOINT_LOCK` and why checkpoint must be called before acquiring the lock is accurate and well-explained. Verified against schema.h:142-153.

2. **Appropriate Placement of Checkpoint**: Calling checkpoint in the caller (`__wti_disagg_conn_config`) rather than modifying `__disagg_step_down` is the right architectural decision - it keeps the existing void function unchanged and handles errors at the appropriate level.

3. **Clear Race Window Documentation**: The race window between checkpoint completion and step-down is honestly documented with clear caller responsibility guidelines.

4. **Backward Compatibility**: The default `local_mode=false` preserves existing behavior and requires explicit opt-in.

5. **Comprehensive Test Strategy**: The proposed tests cover the key scenarios: checkpoint occurrence, data visibility, and idempotency.

6. **Follows Existing Patterns**: The configuration parsing follows the pattern of `lose_all_my_data` and other boolean configs in the disaggregated storage section.

---

## Recommendations

### Before Implementation:

1. **Resolve state storage** - Decide between connection flag vs. disagg struct field for `local_mode`
2. **Fix config parsing order** - Move parsing before role handling, not after reconfig check
3. **Review error atomicity** - Document or improve rollback behavior on checkpoint failure

### During Implementation:

4. **Add dedicated stat** - Consider `disagg_step_down_checkpoint_time` stat for observability
5. **Improve test reliability** - Use data visibility verification instead of checkpoint count

### After Implementation:

6. **Performance testing** - Verify the checkpoint latency estimates in the design against actual measurements with PALite

---

## Verification Checklist

- [x] Read all referenced source files (not just the document)
- [x] Verified thread safety claims against actual lock patterns (schema.h, checkpoint_txn.c)
- [x] Traced memory ownership for all allocations (stack-only, no issues)
- [x] Checked all error paths for proper cleanup (concern raised in Critical #3)
- [x] Confirmed API consistency with existing codebase (follows existing patterns)
- [x] Identified undefined function implementations (all functions exist)
- [x] Verified type consistency throughout (correct types used)
- [x] Assessed test coverage adequacy (adequate but reliability concerns noted)

---

**Review Result**: The design is fundamentally sound but requires addressing the critical issues around state management and configuration parsing order before implementation. The medium issues are important for production quality but should not block initial development.
