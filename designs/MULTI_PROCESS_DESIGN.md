# Mongolite Multi-Process Architecture Design

> **Status**: Forward-looking research - not yet implemented
>
> This document outlines a proposed architecture for enabling SQLite-like multi-process
> semantics in mongolite: multiple concurrent readers with a single writer at a time.

## Prerequisites and Blockers

Before implementation can begin, the following must be resolved:

### 1. PALite Multi-Process Support (BLOCKER)

PALite explicitly does not support multi-process access today:

```cpp
// From palite.cpp "Known Limitations" section:
// PALite does not currently support multiple processes accessing the same
// database files. While SQLite supports this use case, it allows only one
// writer at a time. In absence of a proper locking mechanism between multiple
// processes at PALite level, multiple writers will conflict and fail.
```

This is tracked as **FIXME-WT-16159**. Either:
- Wait for upstream fix, or
- Contribute the fix (add proper inter-process locking at PALite level)

### 2. PALite Durability Setting (CRITICAL)

PALite currently uses `PRAGMA synchronous = OFF` for performance:

```cpp
// From palite.cpp:
"PRAGMA synchronous = OFF;"  // Writes are NOT durable!
```

For mongolite, this must be changed to `NORMAL` or `FULL` to ensure committed
writes survive crashes. This may require a configuration option in PALite or
a mongolite-specific PALite variant.

### 3. demoteToFollower() API Gap

The MongoDB storage layer has `promoteToLeader()` but no corresponding
`demoteToFollower()`. This must be added:

```cpp
// Needed in wiredtiger_kv_engine.cpp:
void WiredTigerKVEngine::demoteToFollower() {
    static constexpr char followerConfig[] = "disaggregated=(role=\"follower\")";
    invariantWTOK(_conn->reconfigure(_conn, followerConfig), nullptr);
}
```

## Key Design Decisions

Based on code analysis and review, these are the critical design decisions:

| Decision | Choice | Rationale |
|----------|--------|-----------|
| **Initial visibility mechanism** | Checkpoint-based | Start simple with existing WiredTiger behavior |
| **Checkpoint timing** | After each write session | Checkpoint before releasing writer lock |
| **Expected latency (initial)** | 15-120ms per write session | Promotion (5-20ms) + checkpoint (10-100ms) |
| **Future optimization** | LSN-based visibility | If checkpoint latency is problematic, implement LSN-based visibility (see Appendix A) |
| **Refresh strategy** | Explicit API + cursor auto-refresh | Don't rely on automatic; give applications control |
| **Connection model** | One connection per process | Matches SQLite; avoids shared memory complexity |

## Implementation Strategy

### Phase 1: Checkpoint-Based Visibility (Initial Implementation)

Start with the simplest correct implementation using existing WiredTiger behavior:

1. Writer acquires lock → promotes to leader
2. Writer performs operations
3. **Writer checkpoints before demoting** (this makes writes visible)
4. Writer demotes to follower → releases lock

This approach:
- Uses existing, well-tested WiredTiger code paths
- Provides correct visibility semantics
- Has predictable latency characteristics

### Phase 2: Benchmark and Evaluate

Before optimizing, measure actual performance:

```
Metrics to collect:
- End-to-end write session latency (p50, p95, p99)
- Checkpoint duration by data volume
- Promotion/demotion overhead
- Reader visibility latency
```

Decision criteria will be determined after seeing real benchmark numbers.

### Phase 3: LSN-Based Visibility (If Needed)

If checkpoint-based visibility proves too slow, implement LSN-based visibility
as described in **Appendix A**. This would provide SQLite WAL-like semantics
with ~1ms visibility latency instead of 10-100ms.

## Table of Contents

1. [Overview](#overview)
2. [Goals and Non-Goals](#goals-and-non-goals)
3. [Background: Disaggregated Storage](#background-disaggregated-storage)
4. [Proposed Architecture](#proposed-architecture)
5. [Key Components](#key-components)
6. [Visibility Model](#visibility-model)
7. [Constraints and Challenges](#constraints-and-challenges)
8. [Cost Analysis](#cost-analysis)
9. [Implementation Phases](#implementation-phases)
10. [Open Questions](#open-questions)
11. [References](#references)

---

## Overview

Mongolite currently operates as a single-process embedded MongoDB. This design proposes
extending mongolite to support multi-process access with SQLite-like semantics:

- **Multiple readers**: Any number of processes can read concurrently
- **Single writer**: Only one process can write at a time
- **No reader blocking**: Writers do not block readers
- **Near-real-time visibility**: Readers see committed writes almost immediately

The key insight is that MongoDB's **disaggregated storage infrastructure** provides
most of the primitives needed for this, and a local implementation called **PALite**
(using SQLite as a backing store) already supports multi-reader/single-writer semantics.

## Goals and Non-Goals

### Goals

1. Enable multiple processes to share a mongolite database
2. Provide SQLite-like locking semantics (readers never blocked)
3. Ensure durability and consistency of writes
4. Minimize latency for writer acquisition
5. Keep the API simple - applications shouldn't need to manage writer state

### Non-Goals

1. High-performance multi-writer workloads (single writer is acceptable)
2. Distributed multi-node access (local filesystem only)
3. Sub-millisecond write latency (embedded use case tolerates some overhead)
4. Backward compatibility with standard MongoDB deployments

## Background: Disaggregated Storage

MongoDB's disaggregated storage clusters (DSC) separate compute from storage using
external Page and Log Services. The key components relevant to mongolite are:

### WiredTiger's WT_PAGE_LOG Interface

WiredTiger defines an abstraction (`WT_PAGE_LOG`) for shared storage:

```c
struct __wt_page_log {
    int (*plh_get)(WT_PAGE_LOG_HANDLE *, WT_SESSION *, ...);   // Read page
    int (*plh_put)(WT_PAGE_LOG_HANDLE *, WT_SESSION *, ...);   // Write page
    int (*plh_discard)(WT_PAGE_LOG_HANDLE *, WT_SESSION *, ...); // Delete page
    // ... checkpoint management methods
};
```

### PALite: Local Page Log Implementation

PALite (`src/third_party/wiredtiger/ext/page_log/palite/palite.cpp`) is a **local**
implementation of `WT_PAGE_LOG` using SQLite as its backing store:

```
dbpath/
├── globals.db      # Global metadata, LSN tracking
├── checkpoints.db  # Checkpoint metadata
└── pages_N.db      # Sharded page storage (N = 0..num_shards-1)
```

**Critical for mongolite**: PALite explicitly uses SQLite's WAL mode and supports
multi-reader/single-writer concurrency:

```cpp
// From palite.cpp
/*
 * -= Concurrency model =-
 *
 * SQLite supports multiple simultaneous read transactions coming from separate
 * database connections, possibly in separate threads or processes, but only one
 * simultaneous write transaction.
 */
```

### Layered Tables

In disaggregated mode, WiredTiger uses "layered tables" that merge two views:

| Component | Description | Location |
|-----------|-------------|----------|
| **Stable Table** | Checkpoint-consistent data | Shared (page log) |
| **Ingest Table** | Recent uncommitted changes | Local (in-memory) |

Cursors automatically merge these views, providing a unified read path.

### Leader/Follower Roles

- **Leader**: Can write to page log, creates checkpoints
- **Follower**: Read-only, sees data via checkpoints and materialized LSN

### last_materialized_lsn

A critical mechanism for reader visibility. The writer updates this LSN after
committing, signaling that pages up to this point are safe to read. Readers'
layered cursors auto-refresh when this changes.

```cpp
// From wiredtiger_kv_engine.cpp
void WiredTigerKVEngine::setLastMaterializedLsn(uint64_t lsn) {
    invariantWTOK(_conn->set_context_uint(_conn, WT_CONTEXT_TYPE_LAST_MATERIALIZED_LSN, lsn),
                  nullptr);
}
```

## Proposed Architecture

### High-Level Design

```
┌─────────────────────────────────────────────────────────────────┐
│                        Process A (Writer)                        │
│  ┌──────────────┐    ┌─────────────────────┐    ┌────────────┐  │
│  │   Mongolite  │───▶│ WriterCoordinator   │───▶│ WiredTiger │  │
│  │     API      │    │ (acquire/release)   │    │  (Leader)  │  │
│  └──────────────┘    └─────────────────────┘    └─────┬──────┘  │
└────────────────────────────────────────────────────────┼────────┘
                                                         │
                              ┌───────────────────────────┼────────┐
                              │         PALite            │        │
                              │  ┌─────────────────────────▼─────┐ │
                              │  │     SQLite (WAL mode)         │ │
                              │  │  - globals.db                 │ │
                              │  │  - checkpoints.db             │ │
                              │  │  - pages_N.db                 │ │
                              │  └───────────────────────────────┘ │
                              └───────────────────────────┬────────┘
                                                         │
┌────────────────────────────────────────────────────────┼────────┐
│                        Process B (Reader)               │        │
│  ┌──────────────┐    ┌─────────────────────┐    ┌──────┴─────┐  │
│  │   Mongolite  │───▶│ WriterCoordinator   │───▶│ WiredTiger │  │
│  │     API      │    │   (read-only)       │    │ (Follower) │  │
│  └──────────────┘    └─────────────────────┘    └────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

### Writer Coordination Flow

```
Application calls write operation
            │
            ▼
┌───────────────────────────────────┐
│  MongoliteWriterCoordinator       │
│  1. Acquire WriterLockFile        │◀── Inter-process flock()
│  2. promoteToLeader()             │◀── WiredTiger reconfigure
│  3. Execute write operations      │
│  4. Checkpoint                    │◀── Required for reader visibility
│  5. demoteToFollower()            │◀── WiredTiger reconfigure
│  6. Release WriterLockFile        │
└───────────────────────────────────┘
```

**Critical:** Step 4 (checkpoint) is required for readers to see writes. Without it,
writes are durable in PALite but invisible to other processes until the next checkpoint.

### Lazy Writer Acquisition

Writers are acquired lazily - applications don't need to declare intent:

```rust
// Proposed API
impl Mongolite {
    pub fn find(&self, ...) -> Result<Cursor> {
        // Always works, never blocks
    }
    
    pub fn insert(&self, doc: Document) -> Result<()> {
        // Internally acquires writer if needed
        self.coordinator.with_writer(|| {
            self.collection.insert_one(doc)
        })
    }
}
```

## Key Components

### 1. MongoliteWriterCoordinator

New component to manage writer mode transitions:

```cpp
// Proposed: src/mongo/mongolite/writer_coordinator.h
class MongoliteWriterCoordinator {
public:
    // Acquire exclusive write access (blocking)
    Status acquireWriter(OperationContext* opCtx);
    
    // Release write access
    void releaseWriter();
    
    // Check if this process is the current writer
    bool isWriter() const;
    
private:
    std::unique_ptr<WriterLockFile> _lockFile;
    std::atomic<bool> _isWriter{false};
};
```

### 2. WriterLockFile

Inter-process lock using file locking:

```cpp
// Proposed: src/mongo/mongolite/writer_lock_file.h
class WriterLockFile {
public:
    explicit WriterLockFile(const std::string& dbpath);
    
    // Acquire exclusive lock (blocks until available)
    Status lock();
    
    // Try to acquire lock (non-blocking)
    StatusWith<bool> tryLock();
    
    // Release lock
    void unlock();
    
private:
    boost::filesystem::path _lockPath;  // dbpath/mongolite_writer.lock
    int _fd{-1};
};
```

### 3. Integration Point: WriteUnitOfWork

The interception point for lazy writer acquisition:

```cpp
// Modified: src/mongo/db/storage/write_unit_of_work.cpp
WriteUnitOfWork::WriteUnitOfWork(OperationContext* opCtx, ...)
    : _opCtx(opCtx), ... {
    
    // NEW: Ensure we have writer access for mongolite
    // Only acquire on top-level WUoW to handle nesting correctly
    if (mongoliteEnabled && _toplevel && !opCtx->isReadOnly()) {
        auto coordinator = MongoliteWriterCoordinator::get(opCtx->getServiceContext());
        if (!coordinator->isWriter()) {
            uassertStatusOK(coordinator->acquireWriter(opCtx));
        }
    }
    
    // ... existing code
}
```

**Important considerations:**

1. **Nested WUoW**: Only acquire on `_toplevel` to avoid deadlock on nested writes
2. **Writer tracking**: Coordinator tracks per-process state, not per-WUoW
3. **Internal writes**: Some internal operations may not set `readOnly()` - need audit
4. **Acquisition timing**: Must happen before `beginWriteUnitOfWork()` on the locker

### 4. MaterializedLsnNotifier

Mechanism to communicate `last_materialized_lsn` between processes:

```cpp
// Proposed: Extend PALite or add mongolite-specific mechanism
class MaterializedLsnNotifier {
public:
    // Writer calls after commit
    void notifyMaterialized(uint64_t lsn);
    
    // Readers poll or receive notification
    uint64_t getLastMaterializedLsn() const;
    
private:
    // Options:
    // 1. Shared memory region
    // 2. File with mmap
    // 3. PALite globals.db table
};
```

## Visibility Model

### CORRECTION: Visibility is Checkpoint-Based, Not LSN-Based

**Important:** Earlier versions of this document incorrectly claimed that `last_materialized_lsn`
drives reader visibility. This is wrong. Code analysis of `cur_layered.c` shows:

```c
// From __clayered_adjust_state():
/* Get the current checkpoint LSN. This only matters if we are a follower. */
if (!current_leader)
    last_checkpoint_meta_lsn =
      __wt_atomic_load_uint64_acquire(&conn->disaggregated_storage.last_checkpoint_meta_lsn);

// Cursor only refreshes when checkpoint_meta_lsn changes
if (current_leader == clayered->leader &&
  last_checkpoint_meta_lsn == clayered->checkpoint_meta_lsn)
    return (0);  // No refresh
```

The `last_materialized_lsn` is used for **eviction control** (preventing eviction of pages
ahead of the materialization frontier), NOT for reader visibility.

### How Readers Actually See Committed Data

Readers see new data when:
1. Writer completes a **checkpoint**
2. `last_checkpoint_meta_lsn` is updated
3. Reader's layered cursor detects the change and refreshes

```
Time ──────────────────────────────────────────────────────────────▶

Writer:  [acquire] [write] [write] [checkpoint] [update ckpt LSN] [release]
                                        │              │
                                        ▼              ▼
PALite:                    [pages written] [checkpoint complete]
                                                       │
Reader:                                                ▼
                                        [cursor detects LSN change, refreshes]
```

This is similar to **SQLite's default (rollback journal) mode**, not WAL mode.
Visibility latency is tied to checkpoint frequency.

### Visibility Guarantees

| Guarantee | Supported | Notes |
|-----------|-----------|-------|
| Read-your-writes | ✅ | Same process sees its own writes immediately |
| Monotonic reads | ✅ | Checkpoint LSN only increases |
| Causal consistency | ⚠️ | Requires checkpoint between write and read |
| Serializable | ✅ | Readers see consistent checkpoint snapshots |
| Low-latency visibility | ❌ | Must wait for checkpoint |

### Visibility Latency Implications

| Checkpoint Frequency | Visibility Latency | Tradeoff |
|---------------------|-------------------|----------|
| Every commit | ~10-100ms | Low latency, high overhead |
| Every N seconds | N seconds | Balanced |
| Manual only | Unbounded | Application controlled |

**Recommendation:** Start with checkpoint-on-writer-release for simplicity. This provides
SQLite-like semantics where readers see data after the writer releases the lock.

### Comparison with SQLite

| Aspect | SQLite Rollback | SQLite WAL | Mongolite (Actual) |
|--------|-----------------|------------|-------------------|
| Reader blocking | During writes | Never | Never |
| Write visibility | After commit | After commit | After checkpoint |
| Checkpoint needed | N/A | For space only | For visibility |
| Latency | Low | Low | Medium (checkpoint-bound) |

### Future Enhancement: LSN-Based Visibility

To achieve SQLite WAL-like low-latency visibility, WiredTiger's layered cursor would
need modification to also check `last_materialized_lsn` for state changes. This is
a potential future enhancement if checkpoint-based visibility proves too slow.

See **Appendix A** for detailed design of LSN-based visibility, including:
- Why it's safe in local mode
- Required WiredTiger modifications
- Testing and verification approach

### Edge Cases

**Crash during checkpoint:**
- Checkpoint is atomic at the WiredTiger level
- Incomplete checkpoint is abandoned on recovery
- Readers continue seeing the previous checkpoint

**Writer crash before checkpoint:**
- Uncommitted writes are rolled back on recovery
- No data loss of committed data (assuming synchronous != OFF)
- Readers see last completed checkpoint

## Constraints and Challenges

### 1. Implicit Writes Must Be Suppressed

MongoDB performs various internal writes even during read operations:

| Source | Location | Mitigation |
|--------|----------|------------|
| Startup logging | `local.startup_log` | Guarded by `supportsLocalCollections()` |
| Session tracking | `config.system.sessions` | Disable LogicalSessionCache |
| TTL Monitor | Various | Disable TTLMonitor |
| WiredTiger SizeStorer | Internal | Use read-only mode flag |
| HealthLog | `local.system.healthlog` | Disable HealthLog |
| System index verification | Various | Skip or make read-only |
| Index builds | `IndexBuildsCoordinator` | Block in reader mode |
| Cursor state (sessions) | Session storage | Disable persistent sessions |
| Collection validation | Various | Block in reader mode |

Reference: `mongod_lite_main.cpp` startup sequence

**Partial protection via `supportsLocalCollections()`:**

The `service_entry_point_shard_role.cpp` blocks writes to `local` database when
`supportsLocalCollections()` returns false:

```cpp
if (dbName == DatabaseName::kLocal &&
    !rss.getPersistenceProvider().supportsLocalCollections()) {
    bool commandIsWrite = ...;
    uassert(ErrorCodes::IllegalOperation,
            "Not allowed to write to 'local' database",
            !commandIsWrite);
}
```

However, this does NOT protect all write paths. A comprehensive audit is required.

**Audit action item:** Before implementation, grep for all `WriteUnitOfWork` usages
in mongod_lite initialization paths and verify each is either:
1. Guarded by a mode check, or
2. Explicitly disabled in reader mode

### 2. Multi-Document Transactions

Multi-document transactions require the `config.transactions` collection for
tracking prepared transactions. Options:

1. **Disable multi-document transactions** in reader mode (simpler)
2. **Allow transactions only when writer** (more complex coordination)
3. **Use in-memory transaction tracking** (research needed)

### 3. Checkpoint vs. Materialized LSN

In current WiredTiger, checkpoints and `last_materialized_lsn` serve different purposes:

| Purpose | Checkpoint | Materialized LSN |
|---------|------------|------------------|
| Reader visibility (current) | ✅ Required | ❌ Not used |
| Eviction control | ❌ Not used | ✅ Required |
| Space reclamation | ✅ Required | ❌ Not sufficient |
| Crash recovery | ✅ Required | ❌ Not sufficient |

**Initial approach**: Use checkpoint-based visibility (existing behavior).

**Future optimization**: If checkpoint latency is problematic, implement LSN-based
visibility as described in **Appendix A**. This would allow `last_materialized_lsn`
to drive reader visibility in local mode.

### 4. Process Crash and Stale Lock Handling

If a writer crashes while holding the lock:

1. Lock file is released by OS on process exit (flock behavior)
2. Next writer acquires lock normally
3. WiredTiger recovery runs on startup
4. PALite/SQLite handles its own recovery

**Stale lock problem:** If a process hangs (not crashed) while holding the lock,
other writers are blocked indefinitely. `flock()` has no built-in timeout.

Mitigation options:

1. **Lock with timeout**: Use `fcntl()` with `F_SETLK` and alarm/signal for timeout
2. **Liveness file**: Writer periodically updates a timestamp file; other processes
   can forcibly break the lock if timestamp is stale
3. **Lease-based locking**: Lock expires after N seconds unless renewed
4. **Process heartbeat**: Separate mechanism to detect hung processes

Recommendation: Start with option 1 (timeout) for simplicity, add option 2 (liveness)
if hung processes become a problem in practice.

### 5. Stale Reader Connections

Readers with long-lived connections may have stale views:

1. Layered cursors auto-refresh on `last_checkpoint_meta_lsn` change (not LSN)
2. Refresh happens on cursor operations, not automatically in background
3. Provide explicit refresh API for applications needing immediate visibility

### 6. Schema Changes

When a writer creates new collections or indexes:

1. Schema metadata is part of the checkpoint
2. Readers see new schema after checkpoint pickup
3. **Concern:** Reader may have cached dhandles for old schema
4. May need connection reopen for major schema changes

### 7. Error Handling

| Error | Cause | Handling |
|-------|-------|----------|
| `SQLITE_BUSY` | PALite contention | Retry with backoff |
| `WT_ROLLBACK` | Transaction conflict on step-down | Abort and retry |
| Lock timeout | Another writer holding lock | Return error to application |
| `promoteToLeader()` failure | WiredTiger state issue | Panic or retry from clean state |

### 8. Connection Lifecycle

- WiredTiger recovery runs at **connection open**, not writer acquisition
- Each process should open its own connection at startup
- Connection remains open across reader/writer transitions
- Recovery of crashed writer's state happens when next process opens connection

## Cost Analysis

### Promotion to Writer (Step-Up)

Step-up is NOT free. Even with empty ingest tables, `__layered_drain_ingest_tables` does:
- Creates thread pool infrastructure
- Iterates through all layered table manager entries
- Creates/destroys work queue structures

| Operation | Cost (Cold) | Cost (Warm) | Notes |
|-----------|-------------|-------------|-------|
| Acquire file lock | ~0ms | ~0ms | flock() syscall |
| Set leader flag | ~0ms | ~0ms | Memory write |
| Restart checkpoint | 5-50ms | 1-10ms | Abandons incomplete ckpt |
| Create stable tables | 10-100ms | ~0ms | Only if missing |
| Drain ingest (empty) | 5-20ms | 1-5ms | Thread pool setup/teardown |
| **Total** | **20-170ms** | **2-15ms** | |

**Realistic estimates by scenario:**

| Scenario | Expected Latency |
|----------|-----------------|
| First promotion after startup | 50-200ms |
| Subsequent promotions (hot cache) | 5-20ms |
| Large dataset (many tables) | 100-500ms |
| Small dataset, warmed up | 2-10ms |

**These are estimates. Actual benchmarking is required.**

### Checkpoint Cost

Since visibility requires checkpointing, this cost is now critical:

| Data Volume Since Last Checkpoint | Checkpoint Time |
|-----------------------------------|-----------------|
| Few KB | 5-20ms |
| Few MB | 20-100ms |
| 100s MB | 100ms-1s |
| GBs | 1-10s |

For per-write-session visibility, total latency = promotion + write + checkpoint + demotion.

### Demotion to Reader (Step-Down)

Step-down itself is fast, but **checkpoint must happen before demote** for visibility:

| Operation | Cost | Notes |
|-----------|------|-------|
| **Checkpoint (before demote)** | **10-100ms+** | Required for reader visibility |
| Set follower flag | ~0ms | Memory write |
| Clear metadata state | ~0ms | Cleanup |
| Release file lock | ~0ms | flock() syscall |
| **Total** | **10-100ms+** | Dominated by checkpoint |

**Important:** `__disagg_step_down` abandons the current incomplete checkpoint. We must
complete a checkpoint **before** calling demote, otherwise writes won't be visible.

**In-flight transactions:** Uncommitted transactions are rolled back on step-down.
The layered cursor returns `WT_ROLLBACK` if there are pending modifications.

### Total Write Session Cost

For a complete write session with visibility:

```
Total = Lock Acquire + Promote + Write + Checkpoint + Demote + Lock Release
      ≈ 0ms + 5-20ms + variable + 10-100ms + 0ms + 0ms
      ≈ 15-120ms + write time
```

This is acceptable for most embedded use cases but not suitable for high-frequency writes.

## Implementation Phases

### Phase 0: Prerequisites (BLOCKING)

1. **Fix PALite multi-process support** (FIXME-WT-16159)
   - Add inter-process locking at PALite level
   - Or wait for upstream fix
2. **Add PALite durability option**
   - Allow `synchronous = NORMAL/FULL` configuration
   - Or create mongolite-specific PALite configuration
3. **Add `demoteToFollower()` API** to WiredTigerKVEngine

### Phase 1: Foundation

1. Configure mongolite to use PALite as page log
2. Start all connections as followers
3. Implement `WriterLockFile` for inter-process coordination (with timeout)
4. Add `MongoliteWriterCoordinator` with basic acquire/release

### Phase 2: Writer Integration

1. Hook `WriteUnitOfWork` to trigger writer acquisition
2. Implement promotion/demotion via WiredTiger reconfigure
3. Suppress implicit writes in reader mode
4. Add `last_materialized_lsn` update after commits

### Phase 3: Reader Visibility

1. Implement `MaterializedLsnNotifier` for inter-process communication
2. Configure layered cursors for follower mode
3. Add cursor refresh on LSN change detection
4. Test multi-process read visibility

### Phase 4: Robustness

1. Handle process crash scenarios
2. Add checkpoint management (background/periodic)
3. Implement graceful degradation for edge cases
4. Performance testing and optimization

### Phase 5: API Refinement

1. Expose writer state in Rust API if needed
2. Add explicit checkpoint API
3. Consider async writer acquisition
4. Documentation and examples

## Testing Strategy

### Multi-Process Test Approaches

1. **Fork-based tests**: Use `fork()` to create child processes sharing the database
2. **Separate process spawning**: Launch multiple mongolite processes via `std::process`
3. **Simulated multi-process**: Single process with multiple connections (limited coverage)

### Failure Injection Points

| Scenario | Injection Point | Expected Behavior |
|----------|-----------------|-------------------|
| Crash during lock acquisition | After flock, before promote | Next process acquires normally |
| Crash mid-write | During WriteUnitOfWork | Recovery on next open |
| Crash after commit, before LSN | After WT commit | Delayed visibility, not data loss |
| Crash during checkpoint | Mid-checkpoint | Checkpoint abandoned, restart |
| Hung process holding lock | Lock timeout | Timeout error, retry or force |
| SQLite BUSY error | PALite operations | Retry with backoff |

### Test Configuration

For durability testing, override PALite's default settings:
```cpp
// Test with full durability to catch bugs
"PRAGMA synchronous = FULL;"
```

### Metrics to Capture

- Writer acquisition latency (p50, p95, p99)
- Lock contention frequency
- Promotion/demotion duration
- Checkpoint frequency and duration
- Reader visibility latency after checkpoint

### Pre-Implementation Verification Tests

Before writing production code, verify these assumptions:

1. **PALite file locking**: Test that `fcntl()` locks work correctly across processes
   on target platforms (Linux, macOS)

2. **Promotion cost benchmark**: Measure actual `promoteToLeader()` time with:
   - Empty database
   - Small database (100 collections, 1MB)
   - Medium database (1000 collections, 100MB)
   - Large database (10000 collections, 1GB)

3. **Checkpoint cost benchmark**: Measure checkpoint time vs. dirty data volume

4. **SQLite WAL visibility**: Verify that SQLite connections in different processes
   see checkpointed data correctly

5. **Recovery behavior**: Test crash scenarios and verify recovery works:
   - Kill writer during write
   - Kill writer during checkpoint
   - Kill writer after checkpoint, before demote

6. **`conn->reconfigure()` overhead**: Profile the reconfigure call path

## Open Questions

### Critical (Must Decide Before Implementation)

#### 1. Checkpoint Strategy

Since visibility is checkpoint-based (not LSN-based), when should checkpoints occur?

Options:
- **Checkpoint before demote**: Every writer session checkpoints before releasing lock
  - Pros: Predictable visibility, simple model
  - Cons: Adds 10-100ms+ to every write session
- **Periodic background**: Checkpoint every N seconds while writer lock held
  - Pros: Amortizes checkpoint cost over multiple writes
  - Cons: Complex, visibility delay up to N seconds
- **Threshold-based**: Checkpoint when dirty data exceeds N MB
  - Pros: Bounds memory usage
  - Cons: Unpredictable visibility latency
- **Manual API**: Application explicitly calls checkpoint
  - Pros: Full control
  - Cons: Pushes complexity to application

**Recommendation**: Start with "checkpoint before demote" for simplicity and predictable
SQLite-like semantics. Optimize later if checkpoint overhead is problematic.

#### 2. Checkpoint Notification Mechanism

How do readers learn that a new checkpoint is available?

Options:
- **Poll `last_checkpoint_meta_lsn`**: Reader cursors already do this on operations
- **Explicit refresh API**: Application calls `mongolite.refresh()` when needed
- **Connection reopen**: Reader closes and reopens connection to see new checkpoint

**Recommendation**: Rely on existing cursor behavior (polls on each operation) plus
explicit refresh API for applications that need immediate visibility.

#### 2. Writer Lock Timeout

What happens when lock acquisition times out?

Options:
- **Return error**: Application decides retry strategy
- **Automatic retry with backoff**: Transparent but may hide issues
- **Configurable timeout**: Default 30s, application can override

**Recommendation**: Configurable timeout with default, return error on timeout.

#### 3. Checkpoint Scheduling

When should checkpoints occur?

Options:
- **On writer release**: Simple but adds latency to every write session
- **Periodic background**: Every N seconds while writer is held
- **Threshold-based**: When page log exceeds N MB
- **Manual only**: Application explicitly requests

**Recommendation**: Threshold-based (e.g., 100MB) with manual override API.

### Important (Should Decide Soon)

#### 4. Reader Refresh Strategy

How should readers detect new data?

Options:
- **Polling**: Simple, 10-100ms latency acceptable for most use cases
- **File watch**: Lower latency, OS-specific complexity
- **Explicit refresh API**: Application controls timing
- **Lazy on next cursor operation**: May miss updates between operations

**Recommendation**: Polling with configurable interval, expose explicit refresh API.

#### 5. Multi-Document Transaction Visibility

Can readers see partial multi-document transactions?

Options:
- **Yes (current design)**: Simpler, matches SQLite behavior
- **No (snapshot isolation)**: Requires transaction tracking, more complex

**Recommendation**: Accept partial visibility for v1, document the limitation.

### Lower Priority (Can Defer)

#### 6. Startup Mode

Should mongolite start as reader or writer by default?

**Recommendation**: Reader by default, matches lazy acquisition philosophy.

#### 7. WiredTiger Configuration

Optimal cache sizes, checkpoint triggers, page log sharding.

**Recommendation**: Use defaults initially, tune based on benchmarks.

### Architecture Alternatives Considered

These alternatives were considered but not adopted:

1. **Shared memory for all page data**: Too complex, PALite+SQLite simpler
2. **Single-process multi-connection**: Doesn't achieve multi-process goal
3. **WAL layer above MongoDB**: Duplicates what PALite already provides
4. **Modify WiredTiger for LSN-based visibility**: Would provide lower latency but
   requires WiredTiger changes; defer to future enhancement

## Recommended Implementation Approach

### Start Simple: Checkpoint-Based Visibility

Accept that visibility is checkpoint-based for the initial implementation:

1. Writer acquires lock → promotes → writes → **checkpoints** → demotes → releases
2. Readers see data after writer releases lock (checkpoint complete)
3. Visibility latency = checkpoint time (10-100ms typical)

This matches existing WiredTiger behavior without requiring modifications.

### Benchmark Before Optimizing

Before implementing LSN-based visibility:

1. Implement checkpoint-based approach
2. Benchmark with realistic workloads
3. Measure actual latency distribution
4. Only optimize if latency is actually problematic

### Explicit Refresh API

Provide explicit control over visibility:

```rust
impl Mongolite {
    /// Refresh view to see latest committed data
    pub fn refresh(&self) -> Result<()>;
    
    /// Check if new data is available without refreshing
    pub fn has_updates_available(&self) -> bool;
}
```

### Connection-Per-Process Model

Each process opens its own WiredTiger connection:
- Avoids shared memory complexity
- Matches how SQLite works
- Recovery happens automatically at connection open

### Future Optimization Path

If checkpoint-based visibility proves insufficient:
- Implement LSN-based visibility per **Appendix A**
- This would reduce visibility latency from 10-100ms to ~1ms
- Requires WiredTiger modifications but is architecturally sound for local mode

## References

### Repository Locations

| Component | Path |
|-----------|------|
| Mongolite entry point | `src/mongo/mongolite/mongod_embed.cpp` |
| Mongolite Rust bindings | `src/mongolite_rust/` |
| PALite implementation | `src/third_party/wiredtiger/ext/page_log/palite/palite.cpp` |
| Disagg storage README | `src/mongo/db/modules/atlas/src/disagg_storage/README.md` |
| WiredTiger layered cursor | `src/third_party/wiredtiger/src/cursor/cur_layered.c` |
| WiredTiger KV engine | `src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp` |
| WriteUnitOfWork | `src/mongo/db/storage/write_unit_of_work.cpp` |
| Disagg persistence provider | `src/mongo/db/modules/atlas/src/disagg_storage/disaggregated_persistence_provider.cpp` |

### Key Functions and Types

| Name | Location | Purpose |
|------|----------|---------|
| `promoteToLeader()` | `wiredtiger_kv_engine.cpp` | Reconfigure WT to leader role |
| `setLastMaterializedLsn()` | `wiredtiger_kv_engine.cpp` | Update visibility LSN |
| `__disagg_step_up()` | `conn_layered.c` | WiredTiger step-up implementation |
| `__disagg_step_down()` | `conn_layered.c` | WiredTiger step-down implementation |
| `__clayered_adjust_state()` | `cur_layered.c` | Cursor refresh on role/LSN change |
| `WriteUnitOfWork` | `write_unit_of_work.h` | RAII write transaction wrapper |

### External Documentation

- [SQLite WAL Mode](https://www.sqlite.org/wal.html) - Inspiration for visibility model
- [WiredTiger Documentation](https://source.wiredtiger.com/) - Storage engine internals

---

## Appendix A: LSN-Based Visibility for Local Mode

This appendix describes an optimization that could be implemented if checkpoint-based
visibility proves too slow. It would provide SQLite WAL-like semantics with ~1ms
visibility latency.

### Background: Two LSNs in WiredTiger Disaggregated Storage

WiredTiger's disaggregated storage tracks two separate LSNs:

| LSN | Purpose | Updated When |
|-----|---------|--------------|
| `last_checkpoint_meta_lsn` | Cursor refresh trigger | Checkpoint completes |
| `last_materialized_lsn` | Eviction/read boundary | Pages confirmed in page log |

Currently, **only `last_checkpoint_meta_lsn` triggers cursor refresh**. The
`last_materialized_lsn` is used only for eviction control and read validation.

### Why LSN-Based Visibility is Safe in Local Mode

In distributed disaggregated storage, checkpoints serve as **coordination points**
between multiple nodes that may have different local state. Without checkpoints,
different nodes could see inconsistent views.

In local mode with PALite, this coordination is unnecessary because:

1. **Single source of truth**: All processes read from the same SQLite files
2. **No replication lag**: Pages written to SQLite are immediately available on disk
3. **SQLite WAL guarantees**: Committed writes are atomic and durable
4. **Total ordering via LSN**: LSN provides a consistent read boundary

When `last_materialized_lsn` is updated in local mode, it means:
> "All pages up to this LSN have been committed to PALite/SQLite and are safe to read"

Since all processes access the same physical files, they can safely read any page
with LSN ≤ `last_materialized_lsn` without waiting for a checkpoint.

### Proposed WiredTiger Modifications

#### 1. Modify `__clayered_adjust_state()` in `cur_layered.c`

Current code only checks `last_checkpoint_meta_lsn`:

```c
// Current implementation
if (!current_leader)
    last_checkpoint_meta_lsn =
      __wt_atomic_load_uint64_acquire(&conn->disaggregated_storage.last_checkpoint_meta_lsn);

if (current_leader == clayered->leader &&
    last_checkpoint_meta_lsn == clayered->checkpoint_meta_lsn)
    return (0);  // No refresh needed
```

Modified to also check `last_materialized_lsn` when in local mode:

```c
// Proposed modification
if (!current_leader) {
    last_checkpoint_meta_lsn =
      __wt_atomic_load_uint64_acquire(&conn->disaggregated_storage.last_checkpoint_meta_lsn);
    
    // NEW: Also check materialized LSN in local mode
    if (F_ISSET(conn, WT_CONN_DISAGG_LOCAL_MODE)) {
        last_materialized_lsn =
          __wt_atomic_load_uint64_acquire(&conn->disaggregated_storage.last_materialized_lsn);
    }
}

// Refresh if checkpoint OR materialized LSN changed (in local mode)
if (current_leader == clayered->leader &&
    last_checkpoint_meta_lsn == clayered->checkpoint_meta_lsn &&
    (!F_ISSET(conn, WT_CONN_DISAGG_LOCAL_MODE) || 
     last_materialized_lsn == clayered->last_materialized_lsn))
    return (0);
```

#### 2. Modify `__clayered_open_stable()` in `cur_layered.c`

Currently opens stable cursor at checkpoint. In local mode, could open at
materialized LSN boundary:

```c
// In local mode, read up to materialized LSN instead of checkpoint
if (F_ISSET(conn, WT_CONN_DISAGG_LOCAL_MODE) && !leader) {
    // Open cursor that can read pages up to last_materialized_lsn
    // instead of requiring a checkpoint
}
```

#### 3. Add Local Mode Configuration

Add a new configuration option to enable local mode:

```c
// In wiredtiger_open configuration
"disaggregated=(local_mode=true,...)"
```

This flag enables LSN-based visibility instead of checkpoint-based.

### Writer Flow with LSN-Based Visibility

```
1. Acquire writer lock
2. Promote to leader
3. Execute write operations
4. Commit transaction (pages written to PALite)
5. Update last_materialized_lsn  ← Readers can now see data
6. Demote to follower
7. Release writer lock

(Checkpoint happens periodically in background for space reclamation)
```

### Inter-Process LSN Communication

For readers to see the updated `last_materialized_lsn`, we need inter-process
communication. Options:

| Mechanism | Latency | Complexity | Recommendation |
|-----------|---------|------------|----------------|
| PALite globals.db | 1-5ms | Low | Start here |
| Shared memory + futex | <1ms | Medium | If polling too slow |
| mmap'd file | 1-2ms | Low | Alternative to globals.db |

**Recommended approach**: Store `last_materialized_lsn` in PALite's `globals.db`.
Readers poll this value periodically or on cursor operations.

### Testing and Verification

#### Correctness Tests

1. **Visibility ordering**: Write A, write B, verify readers see A before B
2. **No phantom reads**: Reader in transaction doesn't see writes after snapshot
3. **Crash recovery**: Kill writer mid-write, verify readers see consistent state
4. **Concurrent access**: Multiple readers during write, verify no corruption

#### Stress Tests

1. **High contention**: Many processes competing for writer lock
2. **Large transactions**: Write 1000s of documents, verify visibility
3. **Rapid writer switching**: Process A writes, B writes, A writes, verify ordering

#### Comparative Tests

1. **Compare with SQLite WAL**: Same workload, verify similar semantics
2. **Compare with checkpoint-based**: Measure latency improvement
3. **Durability verification**: Power-loss simulation, verify no data loss

#### Specific Scenarios to Test

```
Test 1: Basic Visibility
  - Process A writes document D1
  - Process A updates last_materialized_lsn
  - Process B reads and sees D1
  - Expected: B sees D1 without checkpoint

Test 2: Ordering Guarantee
  - Process A writes D1 at LSN 100
  - Process A writes D2 at LSN 200
  - Process A updates materialized LSN to 150
  - Process B reads
  - Expected: B sees D1, does NOT see D2

Test 3: Crash Between Write and LSN Update
  - Process A writes D1
  - Process A crashes before updating LSN
  - Process B reads
  - Expected: B does NOT see D1 (correct, write not committed)

Test 4: Crash After LSN Update
  - Process A writes D1
  - Process A updates LSN
  - Process A crashes
  - Process B reads
  - Expected: B sees D1 (write was committed)
```

### Rollback Plan

If LSN-based visibility proves problematic:

1. Disable `local_mode` flag
2. Fall back to checkpoint-based visibility
3. No data migration needed - checkpoints still work

### Performance Expectations

| Metric | Checkpoint-Based | LSN-Based |
|--------|------------------|-----------|
| Visibility latency | 10-100ms | 1-5ms |
| Write session overhead | High (checkpoint) | Low (LSN update) |
| Background work | None | Periodic checkpoint for space |
| Complexity | Low | Medium |

---

*Last updated: 2026-01-31*
*Status: Research/Design Phase*
