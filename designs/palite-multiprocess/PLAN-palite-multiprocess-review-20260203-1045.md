# Design Review: PLAN-palite-multiprocess.md

**Reviewer**: Claude (AI Design Review)
**Date**: 2026-02-03
**Status**: Review Complete

---

## Critical Issues

### 1. Default Value Change is a Breaking Change

The design proposes changing the default `synchronous` value from `OFF` (0) to `NORMAL` (1). This is a **behavior change** that may affect existing users who rely on the current performance characteristics.

**Source code reference** (palite.cpp lines 812-816):
```cpp
/*
 * Turn Synchronous mode OFF for better performance. We don't care about database corruption
 * in case of OS crash or power failure.
 */
"PRAGMA synchronous = OFF;",
```

**Concern**: The comment explicitly states the design rationale ("we don't care about database corruption"). Changing this default contradicts the original design intent.

**Recommendation**: 
- Keep default as `OFF` (0) for backward compatibility
- Document the `synchronous` option prominently
- Let users explicitly opt-in to durability vs. performance tradeoff
- Alternatively, use `NORMAL` as default but require explicit acknowledgment during upgrade

### 2. Incomplete Verification of Actual PALite Code

The verification tests (`verify_palite_multiprocess.py`) create databases with similar schemas to PALite but **do not test the actual PALite code paths**. Specifically:

- The tests use plain Python `sqlite3` module, not the PALite C++ code
- PALite's retry logic, locking patterns, and error handling are not exercised
- The `std::shared_mutex` behavior within PALite is not tested

**Source code reference** - PALite uses a complex locking hierarchy (palite.cpp):
- Line 1981: `std::shared_mutex store_access;` - storage-wide lock
- Line 943: `std::shared_mutex access;` - per-table lock  
- Line 939: `std::shared_mutex conn_state;` - connection map lock
- Line 1978: `std::shared_mutex pages_access;` - pages map lock

**Concern**: The verification tests pass because they test SQLite directly, but don't validate that PALite's C++ locking correctly interacts with SQLite's inter-process locking.

**Recommendation**: Add integration tests that:
1. Spawn multiple processes running actual WiredTiger with PALite
2. Exercise concurrent read/write operations through PALite's API
3. Verify data consistency across processes

### 3. Missing Discussion of SQLite WAL Checkpoint Interference

SQLite WAL mode has specific behaviors during checkpointing that can affect multi-process access:

- `PRAGMA wal_checkpoint(TRUNCATE)` or `PRAGMA wal_checkpoint(RESTART)` can block readers
- Automatic checkpointing (default: every 1000 pages) may cause unexpected contention
- The design doesn't discuss PALite's current checkpoint behavior or whether modifications are needed

**Concern**: Without explicit checkpoint management, multi-process scenarios may experience unexpected blocking or performance degradation.

**Recommendation**: 
- Document SQLite's WAL checkpoint behavior
- Consider adding explicit checkpoint control to PALite
- Test checkpoint behavior under multi-process load

### 4. Relationship with MULTI_PROCESS_DESIGN.md Not Addressed

The existing `designs/MULTI_PROCESS_DESIGN.md` document lists "PALite Multi-Process Support (FIXME-WT-16159)" as a **BLOCKER** for mongolite. It states:

> "Either wait for upstream fix, or contribute the fix (add proper inter-process locking at PALite level)"

However, this design claims:

> "The original PALite architecture is correct... only needs config + doc changes"

**Concern**: These documents appear to contradict each other. The MULTI_PROCESS_DESIGN.md expects "proper inter-process locking at PALite level" to be added, while this design says no locking changes are needed.

**Recommendation**:
- Reconcile the two documents
- Update MULTI_PROCESS_DESIGN.md to reflect the finding that SQLite already provides inter-process coordination
- Ensure both documents align on the solution approach

---

## Medium Issues

### 5. No Input Validation for `synchronous` Parameter

The design proposes accepting `int32_t synchronous` with values 0, 1, or 2, but doesn't specify behavior for invalid values.

**Proposed implementation** (from design document):
```cpp
int32_t synchronous = 1;
```

**Concern**: What happens if a user specifies `synchronous=5` or `synchronous=-1`?

**Recommendation**: Add validation in `Config::configure_value` or a separate validation step:
```cpp
if (synchronous < 0 || synchronous > 2) {
    throw std::invalid_argument("synchronous must be 0 (OFF), 1 (NORMAL), or 2 (FULL)");
}
```

### 6. `std::format` C++20 Dependency Not Verified

The proposed implementation uses `std::format`:
```cpp
stmts.push_back(std::format("PRAGMA synchronous = {};", config.synchronous));
```

**Verification**: The existing code at line 1603 already uses `std::format`:
```cpp
config_statements.push_back(std::format("PRAGMA mmap_size = {};", MMAP_SIZE));
```

This confirms C++20 is available, so this is not an issue. However, note that the design proposes adding a `std::vector<std::string>` where none currently exists for `config_statements`.

### 7. Connection Class Modification is More Complex Than Described

The design proposes modifying `Connection` to generate config statements dynamically. However, the current implementation (palite.cpp lines 802-829) uses:

```cpp
constexpr static std::string_view config_statements[] = {...};
```

This is a `constexpr static` array that cannot be easily changed to dynamic generation. The design's proposed approach requires:

1. Adding a `Config&` reference to `Connection` 
2. Changing from static array to member function
3. Modifying the constructor signature

**Source code reference** (palite.cpp lines 825-829):
```cpp
Connection(Config &cfg, const std::filesystem::path &db_path)
    : config(cfg), db(open_db(cfg, db_path))
{
    configure(Connection::config_statements);
}
```

**Concern**: The `Connection` class already takes `Config&` in the constructor (named `cfg`) but stores it locally. The design needs to clarify the full change required.

### 8. FIXME-WT-16134 (Test Parallelism) Not Addressed

The design mentions FIXME-WT-16134 but explicitly defers it:

> "Likely NOT related to our fix... Investigate separately; may require test harness changes"

**Source code reference** (test/evergreen.yml lines 1629, 1644):
```yaml
num_jobs: 1 # FIXME-WT-16134 PALite cannot run multiple jobs. PALI will address this.
```

**Concern**: If multi-process support is being enabled, the test parallelism issue should be understood. Multiple test jobs may use the same PALite directory, causing the exact scenario this design claims to fix.

**Recommendation**: 
- Verify whether the test parallelism issue is the same as the multi-process issue
- If so, update the design to address it
- If not, document why they are different

---

## Minor Issues

### 9. Line Number References May Become Stale

The design references specific line numbers (e.g., "around line 343", "lines 802-819") that may change as the code evolves.

**Recommendation**: Use function/class names or stable identifiers instead of line numbers where possible.

### 10. Missing Test Code in Design

The design describes tests in Phase 2 but doesn't include actual test implementations. The testing strategy section (lines 598-657) describes what to test but not how.

**Recommendation**: Add skeleton test files or at least test function signatures to the design.

### 11. Verification Scripts Not Integrated with Test Suite

The verification scripts (`verify_sqlite_multiprocess.py`, `verify_palite_multiprocess.py`) are placed in `designs/palite-multiprocess/` but are not integrated with the WiredTiger test suite.

**Recommendation**: 
- Move verification tests to `test/suite/` after implementation
- Integrate with `test/evergreen.yml` for CI

### 12. Documentation Update Location Not Specified

The design says to update documentation at "Lines 59-65" but WiredTiger may have other documentation (API docs, README) that also mentions PALite limitations.

**Recommendation**: Search for all references to PALite's multi-process limitation and update comprehensively.

---

## Strengths

1. **Correct Core Analysis**: The design correctly identifies that SQLite WAL mode provides inter-process coordination via file locks. This is accurate based on SQLite documentation.

2. **Thorough SQLite Verification**: The verification tests comprehensively validate SQLite's WAL mode behavior for concurrent readers/writers.

3. **Minimal Change Approach**: The design appropriately limits scope to configuration and documentation rather than architectural changes.

4. **Clear Implementation Plan**: The phased approach (Phase 0: Verification, Phase 1: Implementation, Phase 2: Testing) is well-structured.

5. **Performance Data Included**: The measured performance impact of different `synchronous` settings provides concrete data for decision-making.

6. **Two-Layer Locking Model Documentation**: The design clearly explains the separation between intra-process (std::shared_mutex) and inter-process (SQLite WAL) coordination.

---

## Recommendations

### Before Implementation

1. **Reconcile with MULTI_PROCESS_DESIGN.md**: Update both documents to be consistent.

2. **Decide on Default Value**: Make an explicit decision on backward compatibility vs. safety-by-default for the `synchronous` setting.

3. **Create Integration Tests**: Write tests that exercise actual PALite code paths in multi-process scenarios.

### During Implementation

4. **Add Input Validation**: Validate `synchronous` parameter values.

5. **Document Breaking Changes**: If defaults change, document upgrade path.

6. **Test Checkpoint Behavior**: Verify SQLite WAL checkpoint doesn't cause issues.

### After Implementation

7. **Investigate FIXME-WT-16134**: Verify whether test parallelism issue is resolved.

8. **Update All Documentation**: Search for and update all references to PALite limitations.

9. **Benchmark at Scale**: The verification tests use small datasets; validate performance at production scale.

---

## Verification Checklist

- [x] Read all referenced source files (palite.cpp, evergreen.yml, MULTI_PROCESS_DESIGN.md)
- [x] Verified thread safety claims against actual lock patterns in palite.cpp
- [x] Traced memory ownership - no new allocations proposed (int32_t is stack-allocated)
- [x] Checked error paths - existing retry logic (600 retries) is adequate
- [x] Confirmed API consistency - `synchronous` follows existing int32_t pattern (e.g., `verbose`)
- [x] Identified undefined function implementations - `make_config_statements()` needs full definition
- [x] Verified type consistency - int32_t matches SQLite PRAGMA requirements
- [x] Assessed test coverage - verification tests adequate for SQLite, insufficient for PALite

---

**Review Complete**

The design is fundamentally sound in its analysis that SQLite WAL mode provides inter-process coordination. However, the critical issues around default value changes, integration testing gaps, and reconciliation with the broader multi-process design document should be addressed before implementation proceeds.
