# Local Mode Step-Down Checkpoint - Design Document

## Executive Summary

In local disaggregated mode (using PALite with local filesystem), step-down from leader to follower should complete a checkpoint before transitioning to ensure all committed writes are visible to other processes. This eliminates the need for MongoDB-layer coordination and provides SQLite-like visibility semantics where data written by one process becomes visible to others upon leadership release.

## Problem Analysis

### Current State

The `__disagg_step_down` function in `src/conn/conn_layered.c` currently performs a minimal transition:

```c
static void
__disagg_step_down(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    __wt_verbose_debug1(
      session, WT_VERB_DISAGGREGATED_STORAGE, "%s", "Stepping down to the follower mode");

    conn->layered_table_manager.leader = false;
    WT_STAT_CONN_SET(session, disagg_role_leader, 0);

    /* Do some cleanup as we are abandoning the current checkpoint. */
    __disagg_copy_metadata_clear(session);
}
```

Key characteristics:
- Returns `void` (no error handling)
- Abandons incomplete checkpoints via `__disagg_copy_metadata_clear`
- Does NOT force a checkpoint, meaning uncommitted data is invisible to followers
- Called via `WT_WITH_CHECKPOINT_LOCK` macro from `__wti_disagg_conn_config`

### Root Causes

In distributed disaggregated mode, this behavior is correct because:
1. Multiple nodes coordinate via network protocols
2. Visibility is checkpoint-based by design
3. The layer above WiredTiger manages data visibility

In local mode (PALite), this creates a visibility gap:
1. Process A writes data and steps down
2. Process B opens as follower and cannot see A's data
3. Data exists in PALite/SQLite but isn't in a checkpoint
4. Requires external coordination or explicit checkpoint before step-down

### Measurements

Based on `MULTI_PROCESS_DESIGN.md` estimates:

| Scenario | Current Step-Down | With Checkpoint |
|----------|-------------------|-----------------|
| Empty dirty cache | ~0ms | 5-20ms |
| Small dirty data (<1MB) | ~0ms | 10-50ms |
| Large dirty data (>100MB) | ~0ms | 100ms-1s+ |

The additional checkpoint latency is acceptable for the guaranteed visibility semantics.

## Proposed Solution

### Architecture Overview

```
Current Flow:
  reconfigure(role=follower) → __disagg_step_down() → clear metadata → done
                                                   ↑
                                         Data may be invisible

Proposed Flow (local mode):
  reconfigure(role=follower) → [if local_mode: checkpoint()] → __disagg_step_down()
                                       │
                                       ├── success → proceed to step_down
                                       └── failure → return error, remain leader
```

The checkpoint is performed **in the caller** (`__wti_disagg_conn_config`) before invoking `__disagg_step_down`. This approach:
- Avoids lock flag/spinlock desynchronization issues (see Thread Safety section)
- Keeps `__disagg_step_down` unchanged (`void` return, existing behavior)
- Provides clean error handling at the appropriate level

### Data Structures

#### New Flag in `WT_DISAGGREGATED_STORAGE` Struct

Add a `local_mode` flag to the existing `WT_DISAGGREGATED_STORAGE` struct in `src/include/connection.h`.

This is preferable to adding a connection-wide flag because:
1. It groups related disaggregated storage config together
2. The struct already has a `flags` field with `WT_DISAGG_NO_SYNC`
3. Avoids potential issues with the auto-generated connection flags

```c
struct __wt_disaggregated_storage {
    char *page_log;

    /* ... existing fields ... */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_DISAGG_NO_SYNC 0x1u
#define WT_DISAGG_LOCAL_MODE 0x2u  /* Enable local mode semantics for step-down */
    /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t flags;
};
```

The flag indicates that:
- The connection is using disaggregated storage (`__wt_conn_is_disagg()` returns true)
- The page log implementation is PALite or another local implementation
- Step-down should checkpoint before transitioning

#### New Configuration Option

Add to `dist/api_data.py` in `connection_disaggregated_config_common` (around line 148):

```python
connection_disaggregated_config_common = [
    # ... existing configs ...
    Config('lose_all_my_data', 'false', r'''
        This setting skips file system syncs, and will cause data loss outside of a
        disaggregated storage context.''',
        type='boolean', undoc=True),
    Config('local_mode', 'false', r'''
        enable local mode semantics for step-down. When true, a checkpoint is
        performed before stepping down from leader to follower to ensure all
        committed data is visible to other processes.''',
        type='boolean', undoc=True),
    Config('role', '', r'''
        whether the stable table in a layered data store should lead or follow''',
        choices=['leader', 'follower'], undoc=True),
]
```

After running `python dist/s_all.py`, this will generate in `src/include/wiredtiger_open_conf.h`:

```c
#define WT_OPEN_CONF_disaggregated_local_mode  10XX  /* bool: enable local mode semantics... */
```

(Exact ID assigned by generator based on ordering)

The configuration string format:
```
disaggregated=(role="leader",page_log=palite,local_mode=true)
```

### Implementation Details

#### Configuration Parsing

**CRITICAL**: The `local_mode` configuration must be parsed **before** the role handling section (before line 2010) to ensure the flag is set before any step-down logic executes.

Location: `src/conn/conn_layered.c`, in `__wti_disagg_conn_config`, in the "Common settings" section around line 1989:

```c
    /* Common settings between initial connection config and reconfig. */

    /*
     * Parse local_mode setting. This is parsed for both initial connection and reconfig,
     * though the flag only affects behavior during step-down (reconfig from leader to follower).
     * Setting it at initial connection is harmless and keeps the code simple.
     */
    {
        int64_t local_mode_val;
        ret = __layered_config_get_int(session, conn, cfg,
          WT_OPEN_CONF_disaggregated_local_mode, "disaggregated.local_mode",
          &local_mode_val);
        if (ret == 0 && local_mode_val != 0) {
            F_SET(&conn->disaggregated_storage, WT_DISAGG_LOCAL_MODE);
            __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE, "%s",
              "Disaggregated local mode enabled");
        } else if (ret == WT_NOTFOUND) {
            ret = 0;  /* Optional config, default to false */
        }
        WT_ERR(ret);
    }

    /* Get the last materialized LSN. */
    /* ... existing code ... */
```

#### Modified `__wti_disagg_conn_config` Function - Step-Down Path

Location: `src/conn/conn_layered.c`, around line 2033

The checkpoint is performed in the step-down path, **before** acquiring the checkpoint lock:

```c
    } else if (was_leader && !leader) {
        /* Leader step-down. */
        time_start = __wt_clock(session);

        /*
         * In local mode, complete a checkpoint before stepping down to ensure
         * all committed data is visible to other processes. The checkpoint must
         * be performed BEFORE acquiring the checkpoint lock for step-down, because
         * the checkpoint will attempt to acquire the same lock internally.
         *
         * Note: There is a small race window between checkpoint completion and
         * the leader flag being cleared. Writes that occur in this window will
         * not be visible to followers. Callers should ensure no writes are in
         * progress when initiating step-down.
         *
         * If checkpoint fails, we return an error and remain in leader state.
         * The reconfigure operation is NOT atomic - some configuration changes
         * may have been applied before the failure. The caller can retry the
         * step-down or take other recovery action.
         */
        if (F_ISSET(&conn->disaggregated_storage, WT_DISAGG_LOCAL_MODE)) {
            WT_SESSION_IMPL *ckpt_session;
            WT_SESSION *wt_session;
            int ckpt_ret;

            __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE, "%s",
              "Local mode: performing checkpoint before step-down");

            /*
             * Checkpoint cannot run in the default session (session ID 0) because the checkpoint
             * code asserts that it's not running in the default session. We need to create an
             * internal session specifically for the checkpoint operation.
             */
            WT_ERR(__wt_open_internal_session(
              conn, "local-mode-stepdown-checkpoint", true, 0, 0, &ckpt_session));
            wt_session = (WT_SESSION *)ckpt_session;

            ckpt_ret = wt_session->checkpoint(wt_session, "force=true");
            WT_TRET(wt_session->close(wt_session, NULL));

            if (ckpt_ret != 0)
                WT_ERR_MSG_CHK(session, ckpt_ret, "Failed to checkpoint during local mode step-down");
            if (ret != 0)
                WT_ERR_MSG_CHK(
                  session, ret, "Failed to close checkpoint session during local mode step-down");

            __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE, "%s",
              "Local mode: checkpoint complete, proceeding with step-down");
        }

        WT_WITH_CHECKPOINT_LOCK(session, __disagg_step_down(session));
        time_stop = __wt_clock(session);
        WT_STAT_CONN_SET(session, disagg_step_down_time, WT_CLOCKDIFF_MS(time_stop, time_start));
        __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Step down completed in %" PRIu64 " milliseconds",
          WT_CLOCKDIFF_MS(time_stop, time_start));
    }
```

**Implementation Note**: The original design proposed using `__wt_checkpoint_db()` directly. During implementation, it was discovered that the checkpoint code asserts it's not running in the default session (session ID 0), which is what the `reconfigure` API uses. The implementation creates a dedicated internal session for the checkpoint operation, following the pattern used by the checkpoint server thread.

**Note on `disagg_step_down_time` statistic**: When `local_mode=true`, this statistic will include the checkpoint duration. This is intentional as it reflects the total time the step-down operation takes from the caller's perspective. This behavior change should be documented (see Phase 4).

#### `__disagg_step_down` Function (Unchanged)

The `__disagg_step_down` function remains unchanged:

```c
static void
__disagg_step_down(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    __wt_verbose_debug1(
      session, WT_VERB_DISAGGREGATED_STORAGE, "%s", "Stepping down to the follower mode");

    conn->layered_table_manager.leader = false;
    WT_STAT_CONN_SET(session, disagg_role_leader, 0);

    /* Do some cleanup as we are abandoning the current checkpoint. */
    __disagg_copy_metadata_clear(session);
}
```

### Thread Safety

#### Why Checkpoint Must Be In The Caller

The `WT_WITH_CHECKPOINT_LOCK` macro in `src/include/schema.h` implements re-entrant lock semantics using session flags:

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

If `__disagg_step_down` were to release the spinlock with `__wt_spin_unlock()` while still inside `WT_WITH_CHECKPOINT_LOCK`:
1. The spinlock would be released
2. But `WT_SESSION_LOCKED_CHECKPOINT` flag would still be set in `session->lock_flags`
3. When `__wt_checkpoint_db` calls `WT_WITH_CHECKPOINT_LOCK`, the macro would see the flag set
4. It would skip acquiring the spinlock, believing the lock is already held
5. **Result**: Checkpoint would run without the lock actually held - a race condition

By performing checkpoint **before** the `WT_WITH_CHECKPOINT_LOCK` call, we avoid this issue entirely.

#### Race Window Between Checkpoint and Step-Down

There is a small race window in the implementation:

```
Timeline:
[1] Checkpoint begins (acquires checkpoint_lock internally)
[2] Checkpoint completes (releases checkpoint_lock)
[3] --- RACE WINDOW: leader flag still true, new writes possible ---
[4] Step-down acquires checkpoint_lock
[5] leader = false
```

Between steps [2] and [4], another thread could:
- Start a new transaction and write data
- That data won't be in the just-completed checkpoint
- Step-down proceeds, making that data invisible to followers

**Visibility Guarantee**: Data committed **before** the step-down call begins is guaranteed to be visible to followers. Data committed **during** the step-down (in the race window) may not be visible.

**Caller Responsibility**: The MongoDB layer should ensure no writes are in progress when initiating step-down. This is consistent with the expected usage pattern where step-down occurs during a controlled leadership transfer.

#### Lock Ordering

| Operation | Lock Required | Notes |
|-----------|---------------|-------|
| Checkpoint (local mode) | Acquired internally | `__wt_checkpoint_db` acquires checkpoint lock |
| Step-down | `checkpoint_lock` | Via `WT_WITH_CHECKPOINT_LOCK` macro |
| Leader flag modification | `checkpoint_lock` | Protected within step-down |
| Local mode flag read | None | Set once at connection open, read-only thereafter |

#### In-Flight Transaction Handling

The `__wt_checkpoint_db` function internally handles in-flight transactions via `__checkpoint_prepare`:
- Checkpoint waits for running transactions to complete or reach a stable state
- Prepared transactions are handled according to WiredTiger's prepared transaction semantics
- The caller does not need to explicitly wait for transactions

If step-down is called while transactions are active:
- Checkpoint will wait for them (may increase step-down latency)
- Transactions that commit during the checkpoint will be included
- Transactions that commit after checkpoint completes but before step-down may not be visible (race window)

### Memory Management

No new allocations are introduced. The implementation uses:
- Stack variables only (`WT_DECL_RET`, `int64_t local_mode_val`, `const char *checkpoint_cfg[]`)
- Existing session structures for checkpoint

### Error Handling

Error recovery follows a simple model:

| Scenario | Handling |
|----------|----------|
| Checkpoint succeeds | Continue with step-down |
| Checkpoint fails | Return error immediately, remain leader |
| Step-down (after checkpoint) | Always succeeds (void function) |

**Atomicity Note**: The `reconfigure` operation is NOT fully atomic. If checkpoint fails during step-down:
1. Some configuration changes made earlier in `__wti_disagg_conn_config` may have been applied
2. The connection remains in leader state
3. The caller receives an error and can retry or take recovery action
4. No partial step-down state is possible (either fully leader or fully follower)

```c
// Error propagation path:
__wt_checkpoint_db() returns error
    → WT_ERR_MSG_CHK reports error
        → __wti_disagg_conn_config() returns to caller
            → conn->reconfigure() returns error to application
```

### API Changes

#### Public API (No Change)

The public `WT_CONNECTION::reconfigure` API signature is unchanged:
```c
int reconfigure(WT_CONNECTION *connection, const char *config);
```

#### Configuration String

New optional parameter in disaggregated configuration:
```
disaggregated=(role="...",page_log="...",local_mode=true|false)
```

Default: `local_mode=false` (preserves backward compatibility)

#### Return Value Semantics

| Scenario | Return |
|----------|--------|
| Normal step-down (non-local mode) | 0 |
| Step-down in local mode, checkpoint succeeds | 0 |
| Step-down in local mode, checkpoint fails | WT error code |
| Already follower | 0 (no-op) |

## Implementation Phases

### Phase 1: Configuration Infrastructure

1. Add `local_mode` Config entry to `dist/api_data.py` in `connection_disaggregated_config_common`
2. Add `WT_DISAGG_LOCAL_MODE` flag (with `0x0u` placeholder) to `WT_DISAGGREGATED_STORAGE` struct in `src/include/connection.h`
3. Run `python dist/s_all.py` to regenerate:
   - `src/include/wiredtiger_open_conf.h` (adds config key ID)
   - `src/conn/conn_open_conf.c` (config parsing support)
   - `src/include/connection.h` (assigns flag value `0x2u`)
4. Add configuration parsing to `__wti_disagg_conn_config` in the "Common settings" section (BEFORE role handling)

### Phase 2: Core Implementation

1. Add checkpoint logic to `__wti_disagg_conn_config` step-down path with proper config array
2. Add verbose logging for local mode checkpoint operations
3. Test basic functionality

### Phase 3: Testing

1. Add unit test for step-down checkpoint behavior (visibility-based, not count-based)
2. Add multi-process visibility test
3. Add test for step-down with no dirty data (`force=true` verification)
4. Add backward-compatibility test (verify `local_mode=false` does NOT checkpoint)
5. Extend existing `test_palite_multiprocess` tests

### Phase 4: Documentation

1. Update disaggregated storage documentation
2. Document local_mode behavior, visibility guarantees, and race window
3. **Document `disagg_step_down_time` statistic behavior change**: When `local_mode=true`, this statistic includes checkpoint duration, not just the step-down operation itself

## Testing Strategy

### Unit Tests

Location: `test/csuite/test_palite_multiprocess/main.c`

The tests use the existing static `uri` variable defined at file scope:
```c
static const char *uri = "table:test";
static const char *kv_home_dir = "kv_home";
```

Tests follow the existing pattern using `testutil_snprintf` for config strings.

#### Test: `test_local_mode_stepdown_visibility`

The primary test verifies data visibility rather than checkpoint counts (more reliable):

```c
static void
test_local_mode_stepdown_visibility(void)
{
    WT_CONNECTION *conn1, *conn2;
    WT_SESSION *session1, *session2;
    WT_CURSOR *cursor1, *cursor2;
    const char *value;
    char config[1024];
    char home[256], kv_path[256];

    printf("  Testing local_mode_stepdown_visibility\n");

    /* Setup directories following existing test pattern */
    testutil_snprintf(home, sizeof(home), "WT_TEST_local_mode_visibility");
    testutil_recreate_dir(home);
    testutil_snprintf(kv_path, sizeof(kv_path), "%s/%s", home, kv_home_dir);
    testutil_mkdir(kv_path);

    /* Process 1: Open as leader with local_mode, write data, step down */
    testutil_snprintf(config, sizeof(config),
      "create,statistics=(all),"
      "extensions=[%s/ext/page_log/palite/libwiredtiger_palite.so],"
      "disaggregated=(role=\"leader\",page_log=palite,local_mode=true)",
      WT_BUILDDIR);
    testutil_check(wiredtiger_open(home, NULL, config, &conn1));
    testutil_check(conn1->open_session(conn1, NULL, NULL, &session1));
    testutil_check(session1->create(session1, uri,
      "key_format=S,value_format=S,block_manager=disagg"));
    testutil_check(session1->open_cursor(session1, uri, NULL, NULL, &cursor1));
    cursor1->set_key(cursor1, "testkey");
    cursor1->set_value(cursor1, "testvalue");
    testutil_check(cursor1->insert(cursor1));
    testutil_check(cursor1->close(cursor1));

    /* Step down (implicit checkpoint in local mode) - NO explicit checkpoint before */
    testutil_check(conn1->reconfigure(conn1, "disaggregated=(role=\"follower\")"));

    /* Close first connection before opening second (single writer model) */
    testutil_check(session1->close(session1, NULL));
    testutil_check(conn1->close(conn1, NULL));

    /* Process 2: Open as follower (local_mode not needed for follower-only connection) */
    testutil_snprintf(config, sizeof(config),
      "statistics=(all),"
      "extensions=[%s/ext/page_log/palite/libwiredtiger_palite.so],"
      "disaggregated=(role=\"follower\",page_log=palite)",
      WT_BUILDDIR);
    testutil_check(wiredtiger_open(home, NULL, config, &conn2));
    testutil_check(conn2->open_session(conn2, NULL, NULL, &session2));
    testutil_check(session2->open_cursor(session2, uri, NULL, NULL, &cursor2));
    cursor2->set_key(cursor2, "testkey");
    testutil_check(cursor2->search(cursor2));

    testutil_check(cursor2->get_value(cursor2, &value));
    testutil_assert(strcmp(value, "testvalue") == 0);

    testutil_check(cursor2->close(cursor2));
    testutil_check(session2->close(session2, NULL));
    testutil_check(conn2->close(conn2, NULL));
    printf("  PASS: local_mode_stepdown_visibility\n");
}
```

#### Test: `test_local_mode_stepdown_clean_data`

Verify `force=true` creates checkpoint even with no dirty data:

```c
static void
test_local_mode_stepdown_clean_data(void)
{
    WT_CONNECTION *conn1, *conn2;
    WT_SESSION *session1, *session2;
    WT_CURSOR *cursor1, *cursor2;
    const char *value;
    char config[1024];
    char home[256], kv_path[256];

    printf("  Testing local_mode_stepdown_clean_data\n");

    testutil_snprintf(home, sizeof(home), "WT_TEST_local_mode_clean");
    testutil_recreate_dir(home);
    testutil_snprintf(kv_path, sizeof(kv_path), "%s/%s", home, kv_home_dir);
    testutil_mkdir(kv_path);

    /* Open as leader, write data, explicit checkpoint, then step down */
    testutil_snprintf(config, sizeof(config),
      "create,statistics=(all),"
      "extensions=[%s/ext/page_log/palite/libwiredtiger_palite.so],"
      "disaggregated=(role=\"leader\",page_log=palite,local_mode=true)",
      WT_BUILDDIR);
    testutil_check(wiredtiger_open(home, NULL, config, &conn1));
    testutil_check(conn1->open_session(conn1, NULL, NULL, &session1));
    testutil_check(session1->create(session1, uri,
      "key_format=S,value_format=S,block_manager=disagg"));
    testutil_check(session1->open_cursor(session1, uri, NULL, NULL, &cursor1));
    cursor1->set_key(cursor1, "key1");
    cursor1->set_value(cursor1, "value1");
    testutil_check(cursor1->insert(cursor1));
    testutil_check(cursor1->close(cursor1));

    /* Explicit checkpoint - all data is now clean */
    testutil_check(session1->checkpoint(session1, NULL));

    /* Step down with no dirty data - should still work due to force=true */
    testutil_check(conn1->reconfigure(conn1, "disaggregated=(role=\"follower\")"));

    testutil_check(session1->close(session1, NULL));
    testutil_check(conn1->close(conn1, NULL));

    /* Verify data still visible after step-down with clean data */
    testutil_snprintf(config, sizeof(config),
      "statistics=(all),"
      "extensions=[%s/ext/page_log/palite/libwiredtiger_palite.so],"
      "disaggregated=(role=\"follower\",page_log=palite)",
      WT_BUILDDIR);
    testutil_check(wiredtiger_open(home, NULL, config, &conn2));
    testutil_check(conn2->open_session(conn2, NULL, NULL, &session2));
    testutil_check(session2->open_cursor(session2, uri, NULL, NULL, &cursor2));
    cursor2->set_key(cursor2, "key1");
    testutil_check(cursor2->search(cursor2));
    testutil_check(cursor2->get_value(cursor2, &value));
    testutil_assert(strcmp(value, "value1") == 0);

    testutil_check(cursor2->close(cursor2));
    testutil_check(session2->close(session2, NULL));
    testutil_check(conn2->close(conn2, NULL));
    printf("  PASS: local_mode_stepdown_clean_data\n");
}
```

#### Test: `test_local_mode_stepdown_idempotent`

Multiple step-down calls are safe:

```c
static void
test_local_mode_stepdown_idempotent(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_CURSOR *cursor;
    char config[1024];
    char home[256], kv_path[256];

    printf("  Testing local_mode_stepdown_idempotent\n");

    testutil_snprintf(home, sizeof(home), "WT_TEST_local_mode_idempotent");
    testutil_recreate_dir(home);
    testutil_snprintf(kv_path, sizeof(kv_path), "%s/%s", home, kv_home_dir);
    testutil_mkdir(kv_path);

    testutil_snprintf(config, sizeof(config),
      "create,statistics=(all),"
      "extensions=[%s/ext/page_log/palite/libwiredtiger_palite.so],"
      "disaggregated=(role=\"leader\",page_log=palite,local_mode=true)",
      WT_BUILDDIR);
    testutil_check(wiredtiger_open(home, NULL, config, &conn));

    /* Create table and insert data to exercise real checkpoint path */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, uri,
      "key_format=S,value_format=S,block_manager=disagg"));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    cursor->set_key(cursor, "idempotent_key");
    cursor->set_value(cursor, "idempotent_value");
    testutil_check(cursor->insert(cursor));
    testutil_check(cursor->close(cursor));

    /* First step down (triggers checkpoint) */
    testutil_check(conn->reconfigure(conn, "disaggregated=(role=\"follower\")"));

    /* Second step down (no-op, already follower) */
    testutil_check(conn->reconfigure(conn, "disaggregated=(role=\"follower\")"));

    /* Third step down (no-op) */
    testutil_check(conn->reconfigure(conn, "disaggregated=(role=\"follower\")"));

    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));
    printf("  PASS: local_mode_stepdown_idempotent\n");
}
```

#### Test: `test_stepdown_without_local_mode` (Backward Compatibility)

Verify that step-down WITHOUT `local_mode` preserves original behavior (no extra checkpoint):

```c
static void
test_stepdown_without_local_mode(void)
{
    WT_CONNECTION *conn1, *conn2;
    WT_SESSION *session1, *session2;
    WT_CURSOR *cursor1, *cursor2;
    char config[1024];
    char home[256], kv_path[256];
    int ret;

    printf("  Testing stepdown_without_local_mode (backward compatibility)\n");

    testutil_snprintf(home, sizeof(home), "WT_TEST_no_local_mode");
    testutil_recreate_dir(home);
    testutil_snprintf(kv_path, sizeof(kv_path), "%s/%s", home, kv_home_dir);
    testutil_mkdir(kv_path);

    /* Open as leader WITHOUT local_mode */
    testutil_snprintf(config, sizeof(config),
      "create,statistics=(all),"
      "extensions=[%s/ext/page_log/palite/libwiredtiger_palite.so],"
      "disaggregated=(role=\"leader\",page_log=palite)",
      WT_BUILDDIR);
    testutil_check(wiredtiger_open(home, NULL, config, &conn1));
    testutil_check(conn1->open_session(conn1, NULL, NULL, &session1));
    testutil_check(session1->create(session1, uri,
      "key_format=S,value_format=S,block_manager=disagg"));
    testutil_check(session1->open_cursor(session1, uri, NULL, NULL, &cursor1));
    cursor1->set_key(cursor1, "no_local_mode_key");
    cursor1->set_value(cursor1, "no_local_mode_value");
    testutil_check(cursor1->insert(cursor1));
    testutil_check(cursor1->close(cursor1));

    /* Step down WITHOUT local_mode - should NOT checkpoint */
    testutil_check(conn1->reconfigure(conn1, "disaggregated=(role=\"follower\")"));

    testutil_check(session1->close(session1, NULL));
    testutil_check(conn1->close(conn1, NULL));

    /* Open as follower - data should NOT be visible (no checkpoint was done) */
    testutil_snprintf(config, sizeof(config),
      "statistics=(all),"
      "extensions=[%s/ext/page_log/palite/libwiredtiger_palite.so],"
      "disaggregated=(role=\"follower\",page_log=palite)",
      WT_BUILDDIR);
    testutil_check(wiredtiger_open(home, NULL, config, &conn2));
    testutil_check(conn2->open_session(conn2, NULL, NULL, &session2));
    testutil_check(session2->open_cursor(session2, uri, NULL, NULL, &cursor2));
    cursor2->set_key(cursor2, "no_local_mode_key");

    /* Search should fail - data not checkpointed */
    ret = cursor2->search(cursor2);
    testutil_assert(ret == WT_NOTFOUND);

    testutil_check(cursor2->close(cursor2));
    testutil_check(session2->close(session2, NULL));
    testutil_check(conn2->close(conn2, NULL));
    printf("  PASS: stepdown_without_local_mode (backward compatibility)\n");
}
```

### Performance Tests

Measure step-down latency with varying data sizes:

| Data Size | Expected Latency | Acceptable Range |
|-----------|------------------|------------------|
| Empty | 5-20ms | <50ms |
| 1MB dirty | 10-50ms | <100ms |
| 100MB dirty | 100ms-500ms | <1s |
| 1GB dirty | 500ms-2s | <5s |

## Risks and Mitigations

### Risk 1: Checkpoint Latency

**Risk**: Large dirty data volumes could cause step-down to take seconds.

**Mitigation**:
- Document expected latency in API
- MongoDB layer can flush data incrementally before step-down
- Consider adding a timeout option in future if needed

### Risk 2: Checkpoint Failure Modes

**Risk**: Checkpoint could fail for various reasons (disk full, I/O error).

**Mitigation**:
- Clear error propagation to caller
- Connection remains in consistent (leader) state
- MongoDB layer can decide retry policy

### Risk 3: Backward Compatibility

**Risk**: Existing applications might not expect step-down to take longer.

**Mitigation**:
- `local_mode` defaults to `false`
- Explicit opt-in required
- No behavior change unless configured

### Risk 4: Race Window Visibility

**Risk**: Writes during the race window between checkpoint and step-down won't be visible.

**Mitigation**:
- Document the visibility guarantee clearly
- Caller responsibility to quiesce writes before step-down
- Consistent with expected usage pattern (controlled leadership transfer)

## Success Metrics

| Metric | Target |
|--------|--------|
| Step-down with empty cache completes in | <50ms |
| All committed writes visible to follower after step-down | 100% (excluding race window) |
| Step-down failure leaves connection in leader state | 100% |
| No data corruption in crash scenarios | 0 corruption events |
| Backward compatibility with local_mode=false | Identical behavior |

## Appendix A: Files to Modify

| File | Change |
|------|--------|
| `src/include/connection.h` | Add `WT_DISAGG_LOCAL_MODE` flag to `WT_DISAGGREGATED_STORAGE.flags` |
| `dist/api_data.py` | Add `local_mode` to `connection_disaggregated_config_common` |
| `src/conn/conn_layered.c` | Add config parsing (BEFORE role handling), add checkpoint in step-down path |
| `test/csuite/test_palite_multiprocess/main.c` | Add new test functions |

After modifying `dist/api_data.py` and `connection.h`, run:
```bash
python dist/s_all.py
```

This regenerates:
- `src/include/wiredtiger_open_conf.h` (adds `WT_OPEN_CONF_disaggregated_local_mode`)
- `src/conn/conn_open_conf.c` (config parsing support)
- `src/include/connection.h` (assigns correct flag value `0x2u`)

## Appendix B: Related Configuration

Current disaggregated configuration options (from `api_data.py`):

| Name | Default | Description |
|------|---------|-------------|
| `checkpoint_meta` | '' | Checkpoint metadata to start from |
| `drain_threads` | 8 | Thread count for ingest draining |
| `last_materialized_lsn` | '' | Last materialized page LSN |
| `local_files_action` | 'delete' | Action for local files |
| `lose_all_my_data` | false | Skip syncs (unsafe) |
| **`local_mode`** | **false** | **NEW: Enable local mode semantics** |
| `role` | '' | Leader or follower role |
| `page_log` | '' | Page log implementation |

---

*Created: 2026-02-03*
*Updated: 2026-02-03 (incorporated review feedback from reviews 1-4, updated with implementation notes)*
*Status: Implementation Complete - Approved*
