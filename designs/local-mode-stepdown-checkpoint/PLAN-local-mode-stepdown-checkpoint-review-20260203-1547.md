# Design Review: PLAN-local-mode-stepdown-checkpoint.md

**Reviewed**: 2026-02-03

## Critical Issues

### 1. Lock Flag / Spinlock Desynchronization (Thread Safety Bug)

The proposed implementation has a critical bug in the lock handling pattern. The design proposes:

```c
__wt_spin_unlock(session, &conn->checkpoint_lock);
ret = __wt_checkpoint_db(session, NULL, true);
__wt_spin_lock(session, &conn->checkpoint_lock);
```

**The Problem**: The `WT_WITH_CHECKPOINT_LOCK` macro (defined in `src/include/schema.h`) tracks lock ownership via `session->lock_flags`:

```c
#define WT_WITH_LOCK_WAIT(session, lock, flag, op)    \
    do {                                              \
        if (FLD_ISSET(session->lock_flags, (flag))) { \
            op;                                       \  // Already have lock - just run op
        } else {                                      \
            __wt_spin_lock_track(session, lock);      \
            FLD_SET(session->lock_flags, (flag));     \
            op;                                       \
            FLD_CLR(session->lock_flags, (flag));     \
            __wt_spin_unlock(session, lock);          \
        }                                             \
    } while (0)
```

When `__disagg_step_down` is called:
1. The caller `__wti_disagg_conn_config` uses `WT_WITH_CHECKPOINT_LOCK(session, __disagg_step_down(session))`
2. This sets `WT_SESSION_LOCKED_CHECKPOINT` in `session->lock_flags`
3. The proposed code calls `__wt_spin_unlock` which releases the **spinlock** but does NOT clear the **flag**
4. `__wt_checkpoint_db` internally calls `WT_WITH_CHECKPOINT_LOCK(session, ret = __checkpoint_db_wrapper(session, cfg))`
5. The macro sees `WT_SESSION_LOCKED_CHECKPOINT` is set, assumes the lock is held, and executes without acquiring the spinlock
6. **Result**: `__checkpoint_db_wrapper` runs without the checkpoint lock actually held - other threads can acquire the lock and execute concurrently

This is a race condition that could lead to data corruption or undefined behavior.

**Fix**: The checkpoint should be performed **before** entering `__disagg_step_down`, in the caller. The design's "Alternative Design Considered" section actually suggests the correct approach:

```c
// In __wti_disagg_conn_config, BEFORE acquiring checkpoint lock:
} else if (was_leader && !leader) {
    /* Leader step-down. */
    time_start = __wt_clock(session);
    
    /* In local mode, checkpoint before step-down to ensure visibility */
    if (F_ISSET(conn, WT_CONN_DISAGG_LOCAL_MODE)) {
        ret = __wt_checkpoint_db(session, NULL, true);
        if (ret != 0) {
            WT_ERR_MSG_CHK(session, ret, "Failed to checkpoint during local mode step-down");
        }
    }
    
    WT_WITH_CHECKPOINT_LOCK(session, __disagg_step_down(session));
    // ... rest of step-down
}
```

### 2. Non-existent Configuration Helper Function

The design references `__layered_config_get_bool` for parsing configuration:

```c
ret = __layered_config_get_bool(session, conn, cfg,
  WT_OPEN_CONF_disaggregated_local_mode, "disaggregated.local_mode", &local_mode);
```

**This function does not exist.** The codebase only has:
- `__layered_config_get_int` (for integers)
- `__layered_config_get_string` (for strings)

**Fix**: Either:
1. Use `__layered_config_get_int` and cast the result to bool (as done for `lose_all_my_data`)
2. Add a new `__layered_config_get_bool` helper function

The existing pattern for boolean config values uses `__layered_config_get_int`:

```c
int64_t local_mode_val;
WT_ERR(__layered_config_get_int(session, conn, cfg,
  WT_OPEN_CONF_disaggregated_local_mode, "disaggregated.local_mode",
  &local_mode_val));
if (local_mode_val != 0)
    F_SET(conn, WT_CONN_DISAGG_LOCAL_MODE);
```

### 3. Missing api_data.py Configuration Definition

The design mentions updating `dist/api_data.py` but does not provide the actual Config entry. The entry needs to be added to `connection_disaggregated_config_common`:

```python
connection_disaggregated_config_common = [
    # ... existing configs ...
    Config('local_mode', 'false', r'''
        enable local mode semantics for step-down. When true, a checkpoint is
        performed before stepping down from leader to follower to ensure all
        committed data is visible to other processes.''',
        type='boolean', undoc=True),
]
```

## Medium Issues

### 4. Step-Down Return Value Not Propagated

The current code in `__wti_disagg_conn_config` does not capture the return value from `__disagg_step_down`:

```c
WT_WITH_CHECKPOINT_LOCK(session, __disagg_step_down(session));  // void, no return capture
```

Even after changing `__disagg_step_down` to return `int`, the caller needs to be updated to capture the return value properly. The design shows:

```c
WT_WITH_CHECKPOINT_LOCK(session, ret = __disagg_step_down(session));
```

This is correct, but ensure this change is not lost when implementing.

### 5. Checkpoint Failure Leaves Incomplete State

If checkpoint fails during step-down and the connection remains leader:
- The `__disagg_copy_metadata_clear` call in `__disagg_step_down` has not executed
- Any in-progress checkpoint state remains
- The design claims "No partial state visible" but doesn't verify this

**Recommendation**: Verify that remaining leader after failed checkpoint is truly a consistent state, or add explicit cleanup if needed.

### 6. Risk Assessment Gap: "Leader" Check During Checkpoint

The design states:
> Checkpoint operations by other threads are blocked by still being leader

This claim needs verification. I searched for leader-checking logic in checkpoint paths but couldn't find evidence that the leader flag blocks checkpoints. The checkpoint lock itself provides serialization, but if the lock is released (as proposed), other checkpoint requests could proceed.

**Recommendation**: Verify this claim or remove it from the design. If checkpoint operations are not actually blocked by the leader flag, the race window during lock release is more significant than described.

### 7. Configuration ID Placement

The design proposes `WT_OPEN_CONF_disaggregated_local_mode = 1065`. Looking at `wiredtiger_open_conf.h`:
- Core options are in range 1000-1099
- ID 1064 is the last used (`WT_OPEN_CONF_write_through`)
- 1065 would be valid

However, per the comment in `wiredtiger_open_conf.h`, disaggregated options currently live in the core range (1036-1042). Consistency would suggest keeping it there. Verify that 1065 doesn't conflict with any planned additions.

## Minor Issues

### 8. Verbose Message Inconsistency

The proposed code uses `__wt_verbose_debug1` for all verbose messages. The existing codebase uses different verbosity levels:
- `__wt_verbose_debug1` - most messages
- `__wt_verbose_debug2` - more detailed

Consider using `__wt_verbose_debug2` for the intermediate "checkpoint complete, proceeding" message.

### 9. Comment Update Needed

If the alternative design (checkpoint before entering `__disagg_step_down`) is adopted, the existing comment in `__disagg_step_down`:

```c
/* Do some cleanup as we are abandoning the current checkpoint. */
__disagg_copy_metadata_clear(session);
```

May no longer be accurate since in local mode we're completing (not abandoning) the checkpoint. Consider:

```c
/* Clean up checkpoint metadata - completed in local mode, abandoned otherwise. */
__disagg_copy_metadata_clear(session);
```

### 10. Test Code Example Completeness

The test examples show `...` placeholders for statistic queries. Complete examples would be more helpful for implementation. The checkpoint count can be queried via:

```c
WT_STAT_CONN_READ(session, ckpt_generations, ckpt_count_before);
```

## Strengths

1. **Problem Analysis**: Clear explanation of why local mode requires different step-down behavior
2. **Backward Compatibility**: Opt-in via configuration with `local_mode=false` default
3. **Error Handling Model**: Clean error propagation path documented
4. **Testing Strategy**: Comprehensive test cases covering normal operation, visibility, failure, and idempotency
5. **Risk Assessment**: Identifies key risks with mitigations
6. **Phase Plan**: Reasonable implementation phases

## Recommendations

1. **Must Fix Before Implementation**:
   - Restructure to perform checkpoint BEFORE entering `__disagg_step_down` to avoid lock flag desynchronization
   - Use existing `__layered_config_get_int` pattern or create `__layered_config_get_bool`
   - Add the Config entry to `dist/api_data.py`

2. **Should Address**:
   - Verify the claim about leader flag blocking checkpoint operations
   - Add explicit state verification after checkpoint failure

3. **Consider**:
   - Document the expected latency impact in API documentation
   - Add a performance test measuring step-down latency with various dirty data sizes

## Revised Implementation Approach

Instead of modifying `__disagg_step_down` to internally release/re-acquire the lock, modify `__wti_disagg_conn_config`:

```c
} else if (was_leader && !leader) {
    /* Leader step-down. */
    time_start = __wt_clock(session);

    /*
     * In local mode, complete a checkpoint before stepping down to ensure
     * all committed data is visible to other processes.
     */
    if (F_ISSET(conn, WT_CONN_DISAGG_LOCAL_MODE)) {
        __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE, "%s",
          "Local mode: performing checkpoint before step-down");

        ret = __wt_checkpoint_db(session, NULL, true);
        if (ret != 0) {
            __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
              "Local mode step-down checkpoint failed: %s", wiredtiger_strerror(ret));
            WT_ERR_MSG_CHK(session, ret, "Failed to checkpoint during local mode step-down");
        }

        __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE, "%s",
          "Local mode: checkpoint complete, proceeding with step-down");
    }

    WT_WITH_CHECKPOINT_LOCK(session, __disagg_step_down(session));
    time_stop = __wt_clock(session);
    // ... rest unchanged
}
```

This approach:
- Keeps `__disagg_step_down` unchanged (still `void` return)
- Avoids lock/flag desynchronization
- Maintains encapsulation of step-down vs checkpoint concerns
- Error handling is cleaner at the higher level
