# Design Review: PLAN-palite-multiprocess.md (Follow-up Review)

**Reviewer**: Claude (AI Design Review - Fresh Session)  
**Date**: 2026-02-03 16:30  
**Status**: Review Complete  
**Prior Review**: PLAN-palite-multiprocess-review-20260203-1045.md

---

## Summary

This is a follow-up review conducted in a fresh session with no prior context, as requested after critical issues were addressed in the previous review. The design proposes enabling multi-process support in PALite by:

1. Adding a configurable `synchronous` pragma (default: FULL/2)
2. Updating documentation to reflect that multi-process access already works via SQLite WAL
3. Removing the FIXME-WT-16159 comment

**Verdict**: The core analysis is correct - SQLite WAL mode does provide inter-process coordination. However, several issues remain that should be addressed before implementation.

---

## Critical Issues

### 1. Performance Impact of Default Change is Severe and Under-documented

The design changes the default `synchronous` setting from OFF (0) to FULL (2). The measured impact is **~150x slower writes**:

| Setting | Time for 100 writes | Relative |
|---------|---------------------|----------|
| synchronous=OFF | 0.002s | 1.0x (baseline) |
| synchronous=FULL | 0.296s | **~150x slower** |

**Source verification** (palite.cpp lines 812-816):
```cpp
/*
 * Turn Synchronous mode OFF for better performance. We don't care about database corruption
 * in case of OS crash or power failure.
 */
"PRAGMA synchronous = OFF;",
```

**Concerns**:
- The comment explicitly states the design intent ("we don't care about database corruption")
- 150x performance regression is significant and may break existing use cases
- The "Risks and Mitigations" section acknowledges this but classifies it as "High impact" while still proceeding

**Recommendation**: 
- Consider keeping OFF as default for backward compatibility
- Add prominent warning in release notes if changing default
- Document a migration path for existing users who need performance

### 2. Integration Testing Gap Remains Unresolved

The verification tests (`verify_sqlite_multiprocess.py`, `verify_palite_multiprocess.py`) test SQLite behavior and PALite's database schema, but do **not** exercise actual PALite C++ code paths.

The design acknowledges this in line 610:
> "Verification limitation: These tests verify SQLite's behavior and PALite's database schema, but do NOT exercise actual PALite C++ code paths"

**Source verification** - PALite uses complex C++ mechanisms not tested:

| PALite Component | Location | Not Tested |
|-----------------|----------|------------|
| `std::shared_mutex store_access` | line 1981 | Thread-to-process interaction |
| `std::shared_mutex access` (per table) | line 943 | Lock ordering under contention |
| `SQL_CALL_CHECK_RETRIES` retry logic | line 776 | Behavior across processes |
| `PaliteException` error paths | line 513 | Multi-process error propagation |

**Concern**: The fix may work for pure SQLite but fail in the actual PALite environment due to C++ locking interactions.

**Recommendation**: 
- Add Phase 2 integration tests that spawn actual WiredTiger+PALite processes
- Test the specific code path: `PaliteHandle::put()` -> `Storage::put_page()` -> `Pages::put()` -> SQLite
- Verify that `std::shared_mutex` doesn't create deadlocks with SQLite's file locks

### 3. MULTI_PROCESS_DESIGN.md Reconciliation Not Completed

The design mentions updating MULTI_PROCESS_DESIGN.md (lines 377-384) but the actual document still contains contradictory text.

**MULTI_PROCESS_DESIGN.md lines 24-27**:
```
This is tracked as **FIXME-WT-16159**. Either:
- Wait for upstream fix, or
- Contribute the fix (add proper inter-process locking at PALite level)
```

**This design claims** (line 141):
> "We only need: 1. Configuration option for durability 2. Documentation correction"

**Concern**: If both documents are not updated together, future developers may be confused about whether PALite actually needs "proper inter-process locking" or just configuration changes.

**Recommendation**:
- Include specific text for MULTI_PROCESS_DESIGN.md update in this design
- Update both documents in the same commit

### 4. Config Struct Line Number is Incorrect

The design states (line 154):
> "In palite.cpp, struct Config (after line 355, near other int32_t fields)"

**Actual location**: `struct Config` starts at line 343 and the constructor ends at line 380.

**Verified fields** (palite.cpp lines 343-359):
```cpp
struct Config {
    WT_EXTENSION_API *extapi = nullptr;
    std::filesystem::path home_dir;
    uint32_t cache_size_mb = 1'024;
    uint32_t mmap_size_mb = 1'024;
    // ... other uint32_t fields ...
    int32_t verbose = WT_VERBOSE_INFO;  // line 355 - existing int32_t
    bool verbose_msg = true;
    bool sql_trace = false;
    bool verify = true;
};
```

**Recommendation**: Add `int32_t synchronous = 2;` after line 358 (after `verify`), and update line references.

---

## Medium Issues

### 5. Input Validation Location is Ambiguous

The design proposes validation in the constructor (lines 187-192):
```cpp
if (synchronous < 0 || synchronous > 2) {
    throw std::invalid_argument(
        "synchronous must be 0 (OFF), 1 (NORMAL), or 2 (FULL)");
}
```

**Concern**: This validation must happen **after** `configure_value()` is called but within the constructor body. The design doesn't show the exact placement.

Looking at the existing pattern (palite.cpp lines 368-379):
```cpp
configure_value(parser.get(), config, "home", home_dir);
configure_value(parser.get(), config, "cache_size_mb", cache_size_mb);
// ... etc ...
configure_value(parser.get(), config, "verify", verify);
// Constructor body ends - no validation happens here currently
```

**Recommendation**: Show complete constructor modification with validation placement:
```cpp
configure_value(parser.get(), config, "synchronous", synchronous);

// Validate after all configure_value calls
if (synchronous < 0 || synchronous > 2) {
    throw std::invalid_argument(
        "synchronous must be 0 (OFF), 1 (NORMAL), or 2 (FULL)");
}
```

### 6. Connection::config_statements Replacement is Complex

The design proposes replacing the static `constexpr` array with a member function. However, the current implementation pattern is:

**Current** (palite.cpp lines 802-828):
```cpp
class Connection {
    Config &config;  // line 797
    // ...
    constexpr static std::string_view config_statements[] = {...};  // lines 802-819
    
    Connection(Config &cfg, const std::filesystem::path &db_path)
        : config(cfg), db(open_db(cfg, db_path))
    {
        configure(Connection::config_statements);  // line 828
    }
```

**Proposed**: Add `make_config_statements()` member function returning `std::vector<std::string>`.

**Concerns**:
1. The static array must be removed entirely (not just commented out)
2. The `make_config_statements()` function must be declared before the constructor if called there
3. The function returns heap-allocated strings vs. previous static string_views

**Recommendation**: Show the complete class modification including method ordering:
```cpp
class Connection {
private:
    Config &config;
    sqlite3 *db = nullptr;
    std::vector<sqlite3_stmt *> statements;

    // Removed: constexpr static std::string_view config_statements[]
    
    std::vector<std::string> make_config_statements() {
        // Implementation here
    }

public:
    Connection(Config &cfg, const std::filesystem::path &db_path)
        : config(cfg), db(open_db(cfg, db_path))
    {
        configure(make_config_statements());  // Call member function
    }
    // ...
};
```

### 7. WAL Checkpoint Behavior Under Multi-Process Load

The design mentions WAL checkpointing (lines 296-300) but doesn't address how PALite's usage might affect checkpoint behavior.

**SQLite WAL checkpointing facts**:
- Auto-checkpoint occurs when WAL file exceeds ~1000 pages
- `PRAGMA wal_checkpoint(PASSIVE)` is non-blocking
- `PRAGMA wal_checkpoint(TRUNCATE)` can block readers

**PALite current behavior** (not explicitly setting checkpoint mode):
- SQLite defaults to `PRAGMA wal_autocheckpoint = 1000`
- Under high multi-process write load, WAL may grow unbounded if checkpoints are blocked

**Recommendation**: 
- Document expected WAL growth under multi-process load
- Consider adding explicit checkpoint configuration option
- Test WAL size under sustained multi-process writes

### 8. FIXME-WT-16134 Relationship Still Unclear

The design mentions FIXME-WT-16134 (lines 621-631) but explicitly defers investigation:
> "After implementing this fix, test whether `num_jobs > 1` works with PALite. If not, investigate test harness isolation."

**Verified in test/evergreen.yml** (lines 1629, 1644):
```yaml
num_jobs: 1 # FIXME-WT-16134 PALite cannot run multiple jobs. PALI will address this.
```

**Concern**: If the test parallelism issue is caused by the same multi-process problem this design claims to fix, the test should be updated. If it's a different issue (test isolation), the design should clarify.

**Recommendation**: 
- Attempt to run PALite tests with `num_jobs > 1` after implementation
- Document result in design or follow-up issue

---

## Minor Issues

### 9. Verification Scripts Should Move to test/suite

The design states (line 612):
> "Post-implementation: Move verification scripts to `test/suite/` and integrate with CI."

**Recommendation**: Include this as an explicit Phase 2 task, not post-implementation.

### 10. Config Formatter Update Not Shown

The design mentions updating the `std::formatter<Config>` (lines 197-202) but doesn't show the complete change.

**Current** (palite.cpp lines 449-467):
```cpp
template <> struct std::formatter<Config> {
    // ... format string doesn't include synchronous ...
};
```

**Recommendation**: Show complete updated format string for debugging/logging.

### 11. Environment Variable Override Not Documented for synchronous

The design shows environment variable usage (lines 368-374) but the existing `WT_PALITE_CONFIG` environment variable handling (palite.cpp lines 393-398) should work automatically for `synchronous`.

**Recommendation**: Add explicit test case for:
```bash
export WT_PALITE_CONFIG="synchronous=0"
```

---

## Strengths

1. **Correct Core Analysis**: The finding that SQLite WAL mode provides inter-process coordination is accurate and well-documented with verification tests.

2. **Thorough Verification**: The verification scripts comprehensively test SQLite's multi-process behavior.

3. **Minimal Change Approach**: The design correctly identifies that only configuration and documentation changes are needed, not architectural changes.

4. **Performance Data Included**: The measured performance impact provides concrete data for decision-making.

5. **Clear Two-Layer Locking Documentation**: The diagram at lines 52-73 clearly explains the separation of concerns.

6. **Readiness Assessment Added**: The design now includes explicit acknowledgment of issues and next steps (lines 766-786).

7. **Input Validation Added**: The previous review's concern about validation is addressed.

---

## Verification Checklist

- [x] Read all referenced source files (palite.cpp verified line-by-line)
- [x] Verified thread safety claims against actual lock patterns
- [x] Confirmed `synchronous = OFF` is current setting (line 816)
- [x] Confirmed `std::shared_mutex` usage matches design description
- [x] Verified FIXME-WT-16159 exists at line 851
- [x] Checked MULTI_PROCESS_DESIGN.md still has contradictory text
- [x] Verified Config struct location (starts line 343, not 355)
- [x] Confirmed `configure()` template works with `std::vector<std::string>` (used in Pages class)
- [x] Verified `int32_t` type is used elsewhere in Config (line 355: `verbose`)
- [ ] Integration tests exercising actual PALite code paths - NOT PRESENT

---

## Recommendations

### Before Implementation

1. **Decide on Default Value Strategy**:
   - Option A: Keep OFF as default, document `synchronous=2` for multi-process durability
   - Option B: Change to FULL, document `synchronous=0` for backward compatibility
   - Whichever is chosen, update release notes prominently

2. **Reconcile MULTI_PROCESS_DESIGN.md**: Add specific update text to this design.

3. **Fix Line Number Reference**: Config struct starts at line 343.

### During Implementation

4. **Show Complete Code Changes**: The current pseudocode examples should be replaced with exact diffs.

5. **Update Config Formatter**: Include `synchronous` in logging output.

6. **Test Environment Variable Override**: Verify `WT_PALITE_CONFIG="synchronous=N"` works.

### After Implementation

7. **Integration Tests Required**: Test actual WiredTiger+PALite multi-process scenarios before declaring the issue fixed.

8. **Test FIXME-WT-16134**: Attempt `num_jobs > 1` for PALite tests.

9. **Move Verification Scripts**: Integrate into test/suite for CI.

---

## Conclusion

The design's core thesis is correct: SQLite WAL mode already provides inter-process coordination, and PALite's "Known Limitations" comment is overly conservative. The proposed solution (add configurable `synchronous` pragma, update documentation) is appropriate.

However, the **150x performance regression** when changing the default to FULL is significant and should be carefully considered. The **integration testing gap** means we cannot be certain the fix works in production PALite code paths until Phase 2 testing is complete.

**Recommendation**: Proceed with implementation, but:
1. Consider keeping OFF as default for backward compatibility
2. Add integration tests as a blocking requirement before closing FIXME-WT-16159
3. Update MULTI_PROCESS_DESIGN.md in the same commit

---

**Review Complete**

This review path: `designs/palite-multiprocess/PLAN-palite-multiprocess-review-20260203-1630.md`
