# Design Review: Local Mode Step-Down Checkpoint

**Document Reviewed**: `PLAN-local-mode-stepdown-checkpoint.md`  
**Review Date**: 2026-02-03  
**Reviewer**: Independent Design Review

---

## Critical Issues

### 1. Bug: NULL Config Passed to `__wt_checkpoint_db`

**Location**: Implementation Details, Modified `__wti_disagg_conn_config` Function

The design proposes:
```c
ret = __wt_checkpoint_db(session, NULL, true);
```

**Problem**: This will crash or produce undefined behavior. The `__wt_checkpoint_db` function calls `__wt_config_gets` to parse configuration options:

```1794:1795:src/checkpoint/checkpoint_txn.c
    WT_RET(__wt_config_gets(session, cfg, "debug.checkpoint_cleanup", &cval));
```

All existing callers of `__wt_checkpoint_db` in the codebase use a properly initialized config array:

```272:272:src/session/session_compact.c
    const char *checkpoint_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_checkpoint), NULL, NULL};
```

```37:38:src/live_restore/live_restore_server.c
    const char *force_ckpt_cfg[] = {
      WT_CONFIG_BASE(session, WT_SESSION_checkpoint), "force=true", NULL};
```

**Fix Required**: The implementation must use:
```c
const char *checkpoint_cfg[] = {
    WT_CONFIG_BASE(session, WT_SESSION_checkpoint), NULL};
ret = __wt_checkpoint_db(session, checkpoint_cfg, true);
```

Or for a forced checkpoint:
```c
const char *checkpoint_cfg[] = {
    WT_CONFIG_BASE(session, WT_SESSION_checkpoint), "force=true", NULL};
ret = __wt_checkpoint_db(session, checkpoint_cfg, true);
```

Consider whether `force=true` is needed to ensure checkpoint happens even on clean data.

---

## Medium Issues

### 2. Flag Placement in Auto-Generated Section

**Location**: Data Structures, New Connection Flag

The document proposes adding `WT_CONN_DISAGG_LOCAL_MODE` manually within an auto-generated flag section:

```c
#define WT_CONN_WAS_BACKUP 0x4000u
#define WT_CONN_DISAGG_LOCAL_MODE 0x8000u  /* NEW: Local disaggregated mode */
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
```

**Problem**: The `AUTOMATIC FLAG VALUE GENERATION` markers indicate these values are maintained by `dist/flags.py`. Manual insertion risks:
1. Value collision with future auto-generated flags
2. Being overwritten when `dist/s_all.py` runs

**Actual code structure** (`connection.h` lines 1045-1062):
```c
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CONN_BACKUP_PARTIAL_RESTORE 0x0001u
...
#define WT_CONN_WAS_BACKUP 0x4000u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    wt_shared uint32_t flags;
```

**Recommendation**: Either:
1. Add the flag definition to `dist/flags.py` so it's properly managed, or
2. Use a separate flag field outside the auto-generated section (like `debug_flags` or create a new disagg-specific flag field)

### 3. Race Window Between Checkpoint and Step-Down

**Location**: Thread Safety section

The design correctly places checkpoint before acquiring the checkpoint lock for step-down. However, there's an unanalyzed race window:

```
Timeline:
[1] Checkpoint begins (acquires checkpoint_lock internally)
[2] Checkpoint completes (releases checkpoint_lock)
[3] --- RACE WINDOW: leader flag still true, new writes possible ---
[4] Step-down acquires checkpoint_lock
[5] leader = false
```

Between [2] and [4], another thread could:
- Start a new transaction and write data
- That data won't be in the just-completed checkpoint
- Step-down proceeds, making that data invisible to followers

**Recommendation**: Document this window and determine if it's acceptable. Options:
1. Accept the limitation (document that only data committed before checkpoint call is guaranteed visible)
2. Add synchronization to prevent new writes during the checkpoint-to-stepdown transition
3. Loop: checkpoint, check for new writes, repeat if needed

### 4. Missing `force=true` for Checkpoint

**Location**: Implementation Details

The design doesn't specify whether the step-down checkpoint should use `force=true`. Without it, if all data is already checkpointed, `__wt_checkpoint_db` may be a no-op.

Consider the scenario:
1. User calls `session->checkpoint(session, NULL)` explicitly
2. Immediately calls `reconfigure(role="follower")`
3. The step-down checkpoint finds no dirty data
4. This is fine, but should be explicitly documented

**Recommendation**: Consider using `force=true` to ensure metadata consistency, or document why it's not needed.

### 5. Configuration Parsing Uses Non-Existent Key ID

**Location**: Configuration Parsing section

The document shows:
```c
ret = __layered_config_get_int(session, conn, cfg,
  WT_OPEN_CONF_disaggregated_local_mode, "disaggregated.local_mode",
  &local_mode_val);
```

The key `WT_OPEN_CONF_disaggregated_local_mode` doesn't exist until `dist/s_all.py` regenerates the config files. The document mentions this in Phase 1 but doesn't show the expected generated output.

Current keys in `wiredtiger_open_conf.h`:
```c
#define WT_OPEN_CONF_disaggregated_checkpoint_meta      1036
#define WT_OPEN_CONF_disaggregated_drain_threads        1037
#define WT_OPEN_CONF_disaggregated_last_materialized_lsn 1038
#define WT_OPEN_CONF_disaggregated_local_files_action   1039
#define WT_OPEN_CONF_disaggregated_lose_all_my_data     1040
#define WT_OPEN_CONF_disaggregated_role                 1041
#define WT_OPEN_CONF_disaggregated_page_log             1042
```

**Recommendation**: Add the expected generated key ID to the document for completeness.

---

## Minor Issues

### 6. Test Statistics Cursor Usage After Step-Down

**Location**: Testing Strategy, `test_local_mode_stepdown_checkpoints`

The test opens a statistics cursor after step-down:
```c
/* Step down - should trigger checkpoint in local mode */
testutil_check(conn->reconfigure(conn, "disaggregated=(role=\"follower\")"));

/* Get checkpoint count after */
testutil_check(session->open_cursor(session, "statistics:", NULL, NULL, &cursor));
```

This should work, but it's worth verifying that statistics cursors function correctly in follower mode.

### 7. In-Flight Transaction Handling Not Addressed

**Location**: Thread Safety section

The document doesn't describe behavior when there are active transactions at step-down time:
- Should the checkpoint wait for transactions to complete?
- Should step-down fail if there are uncommitted transactions?
- What happens to prepared transactions?

WiredTiger checkpoints typically wait for running transactions via `__checkpoint_prepare`, but the design should explicitly state the expected behavior.

### 8. Test Isolation Concern

**Location**: Testing Strategy, `test_local_mode_stepdown_visibility`

The test opens two connections to the same home directory:
```c
testutil_check(wiredtiger_open(home, NULL, ..., &conn1));
// conn1 writes data and steps down
testutil_check(wiredtiger_open(home, NULL, ..., &conn2));
```

With PALite, this requires careful handling of the shared kv_home. The existing `test_palite_multiprocess` tests use symlinks for this. Verify this test follows the same pattern or can work with direct reopen.

---

## Strengths

1. **Correct Analysis of Lock Semantics**: The document accurately identifies the `WT_WITH_CHECKPOINT_LOCK` re-entrancy issue and correctly places checkpoint before the lock acquisition.

2. **Clean Error Handling Model**: The error handling approach (checkpoint failure keeps leader state, allowing retry) is sound and follows WiredTiger patterns.

3. **Backward Compatibility**: Defaulting `local_mode` to `false` ensures no behavior change for existing users.

4. **Thorough Existing Code Analysis**: The document demonstrates accurate understanding of `__disagg_step_down`, `__disagg_copy_metadata_clear`, and the configuration system.

5. **Good Test Coverage Plan**: The proposed tests cover the key scenarios: checkpoint triggering, data visibility, and idempotent behavior.

---

## Recommendations

### Before Implementation

1. **Fix the NULL config bug** - This is a show-stopper that would cause crashes.

2. **Resolve flag placement** - Either use `dist/flags.py` or a separate flag field.

3. **Document the race window** - Explicitly state the visibility guarantee (data committed before step-down call, not during).

### Implementation Order (Revised)

1. Add `local_mode` to `dist/api_data.py` in `connection_disaggregated_config_common`
2. Add flag to `dist/flags.py` (preferred) or to a non-auto-generated section
3. Run `python dist/s_all.py` to regenerate config code
4. Add configuration parsing with proper `WT_NOTFOUND` handling
5. Add checkpoint call with **proper config array** (not NULL)
6. Add verbose logging
7. Test with existing `test_palite_multiprocess` infrastructure

### Testing Additions

Consider adding tests for:
- Step-down with active transactions
- Step-down with dirty data vs clean data
- Step-down during ongoing checkpoint (should wait or fail cleanly)
- Concurrent step-down requests

---

## Verification Checklist

- [x] Read all referenced source files
- [x] Verified thread safety claims against actual lock patterns
- [x] Traced memory ownership for all allocations (no new allocations, uses stack only - verified)
- [x] Checked all error paths for proper cleanup (WT_ERR pattern is correct)
- [x] Confirmed API consistency with existing codebase
- [x] Identified undefined function implementations (none - uses existing functions)
- [x] Verified type consistency throughout
- [x] Assessed test coverage adequacy (good coverage, minor improvements suggested)

---

**Summary**: The design is fundamentally sound but has one critical implementation bug (NULL config) that must be fixed. The flag placement strategy should be reconsidered to avoid conflicts with auto-generation. The threading analysis is good but should document the race window between checkpoint completion and leader flag change.
