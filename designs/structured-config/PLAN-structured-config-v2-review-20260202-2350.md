# Design Document Review: Unified Structured Configuration System

**Document Reviewed:** `PLAN-structured-config-v2.md` (v1.4)  
**Review Date:** 2026-02-02 23:50  
**Reviewer:** Claude (Design Doc Reviewer)

---

## Review Summary

This is a well-structured design document that has clearly evolved through multiple review cycles. The core concepts are sound, and the document shows good awareness of WiredTiger internals. However, verification against the actual source code reveals several issues that must be addressed before implementation.

---

## Critical Issues

These must be fixed before implementation begins.

### 1. Incorrect `__wt_collator_config` Call Signature

**Location:** Section 3.4.2 (`__btree_conf_from_cache`)

**Document shows:**
```c
WT_ERR(__wt_collator_config(session, btree->uri, &cval, 
    &btree->collator, &btree->collator_owned));
```

**Actual signature (conn_api.c:157-158):**
```c
int __wt_collator_config(WT_SESSION_IMPL *session, const char *uri, WT_CONFIG_ITEM *cname,
  WT_CONFIG_ITEM *metadata, WT_COLLATOR **collatorp, int *ownp)
```

**Problem:** The document omits the `metadata` parameter. The actual call in `bt_handle.c` (lines 528-530) shows:
```c
WT_RET(__wt_collator_config(session, btree->dhandle->name, &cval, &metadata,
  &btree->collator, &btree->collator_owned));
```

**Impact:** The proposed code will not compile.

**Fix:** Update the call to include `metadata`. Since the cache stores only the collator name, you'll need to either:
- a) Also cache `app_metadata` and pass it, OR
- b) Pass NULL for metadata (verify this is safe), OR  
- c) Re-read `app_metadata` from config during cache application

### 2. Non-Existent Function `__wt_btree_config_encryptor_from_cache`

**Location:** Section 3.4.2

**Document shows:**
```c
WT_ERR(__wt_btree_config_encryptor_from_cache(
    session, conf->encryption_name, conf->encryption_keyid, &btree->kencryptor));
```

**Problem:** This function does not exist. The actual function is `__wt_btree_config_encryptor` (bt_handle.c:416-443) which takes `const char **cfg`:

```c
int __wt_btree_config_encryptor(
  WT_SESSION_IMPL *session, const char **cfg, WT_KEYED_ENCRYPTOR **kencryptorp)
```

**Impact:** The proposed code will not compile, and encryptor configuration will fail.

**Fix:** Either:
- a) Create the new helper function `__wt_btree_config_encryptor_from_cache` that accepts individual name/keyid strings, OR
- b) Reconstruct a minimal cfg array from cached strings to call the existing function

### 3. Union Structure Inconsistency in `WT_CURSOR_CONFIG_ARG`

**Location:** Section 4.2.1

**Document shows (line ~877-886):**
```c
typedef struct __wt_cursor_config_arg {
    uint64_t key;
    union {
        int64_t v_int;
        struct { const char *str; size_t len; } v_str;
    } value;
    uint8_t type;
} WT_CURSOR_CONFIG_ARG;
```

**But then shows (line ~900):**
```c
#define WT_CURSOR_CONFIG_ARG_SET_BOOL(k, v) \
    { .key = (k), .value.v_int = (v) ? 1 : 0, .type = WT_CURSOR_CONFIG_ARG_BOOL }
```

**Problem:** The inner struct for strings uses `.str` and `.len`, but the actual existing pattern in wiredtiger.h.in (line 3630) uses:
```c
{ .key = (key_id), .value.v_str = {.str = (s), .len = (slen)}, .type = WT_OPEN_CONFIG_ARG_STR }
```

The document's parsing code (Section 4.3.2 line ~1090) incorrectly uses:
```c
config->checkpoint = args[i].v_str.str;  // WRONG: should be args[i].value.v_str.str
```

**Impact:** Parsing code will not compile.

**Fix:** Change all references from `args[i].v_str.str` to `args[i].value.v_str.str` throughout Section 4.3.2.

---

## Medium Issues

Should be addressed before implementation.

### 4. Missing `app_metadata` in Collator Cache Handling

**Location:** Section 3.4.2, Section 3.5

The document caches `collator_name` but the actual collator configuration (bt_handle.c:526-530) also requires `app_metadata`:

```c
WT_RET(__wt_config_gets_none(session, cfg, "collator", &cval));
if (cval.len != 0) {
    WT_RET(__wt_config_gets(session, cfg, "app_metadata", &metadata));
    WT_RET(__wt_collator_config(session, btree->dhandle->name, &cval, &metadata,
      &btree->collator, &btree->collator_owned));
}
```

**Impact:** Tables with custom collators that use `app_metadata` may behave incorrectly.

**Recommendation:** Either cache `app_metadata` alongside `collator_name`, or document that this is a known limitation where collator lookup still reads `app_metadata` from dhandle->cfg.

### 5. Derived Values Not Documented Correctly

**Location:** Section 3.3.1

The document correctly notes that `maxleafkey` and `maxleafvalue` are derived, but doesn't mention several other values computed in `__btree_page_sizes()` that should NOT be cached:

- `splitmempage` (computed from maxmempage, bt_handle.c:1211)
- `maxintlpage_precomp` / `maxleafpage_precomp` (computed based on compressor, bt_handle.c:700-720)
- `intlpage_compadjust` / `leafpage_compadjust` (computed based on compressor)

**Impact:** If these were accidentally cached, the compression adjustment logic would break.

**Recommendation:** Add explicit note that `__btree_page_sizes()` and the compression adjustment code (lines 699-720) must still run after loading from cache.

### 6. Error Path in `__btree_conf_from_cache` Incomplete

**Location:** Section 3.4.2

The error cleanup code shows:
```c
err:
    __wt_free(session, btree->key_format);
    __wt_free(session, btree->value_format);
    btree->key_format = NULL;
    btree->value_format = NULL;
    return (ret);
```

**Problem:** If collator or compressor lookup fails AFTER strings are copied, additional cleanup may be needed. The actual `__btree_conf` doesn't do this because it relies on `__btree_clear()` on error paths, but the cache path needs to ensure btree state is consistent.

**Recommendation:** Clarify that on error, the caller (`__btree_conf`) should still call the normal error cleanup, or ensure the cache application code leaves btree in a known-good state for cleanup.

### 7. Statistics Naming Convention

**Location:** Section 9.1

The proposed statistics names:
```c
WT_STAT_CONN_BTREE_CONF_CACHE_HIT
WT_STAT_CONN_BTREE_CONF_CACHE_MISS
```

Existing cursor cache statistics use (wiredtiger.h.in):
```c
#define WT_STAT_CONN_CURSOR_CACHED_COUNT  1467
#define WT_STAT_CONN_CURSOR_CACHE         1479
```

The naming is close but verify consistency with existing patterns. The proposed names look reasonable but should be verified against the actual stat.h generation.

---

## Minor Issues

Polish items that should be addressed.

### 8. Type Definition Location Unclear

**Location:** Section 3.3.1

The document shows `WT_BTREE_CHECKSUM checksum;` in the cache structure, but doesn't clarify where `WT_BTREE_CHECKSUM` is defined. It's actually an enum in btree.h (lines 84-89):

```c
typedef enum {
    CKSUM_ON = 1,
    CKSUM_OFF = 2,
    CKSUM_UNCOMPRESSED = 3,
    CKSUM_UNENCRYPTED = 4
} WT_BTREE_CHECKSUM;
```

**Recommendation:** Add a note that `WT_BTREE_CHECKSUM` is already defined in btree.h and can be reused.

### 9. Missing `prefix_compression_min` Initial Value

**Location:** Section 3.3.1

The document includes `prefix_compression_min` in the cache but doesn't show it being set in `__btree_conf_from_cache`. It should be:
```c
btree->prefix_compression_min = conf->prefix_compression_min;
```

### 10. Debug Mode Flag Name Inconsistency

**Location:** Section 3.8

The document proposes `WT_CONN_DEBUG_BTREE_CONF_CACHE_OFF` but the existing debug flag pattern in the codebase uses slightly different naming (e.g., `WT_CONN_DEBUG_CURSOR_COPY`).

**Recommendation:** Verify the flag naming against existing debug flags and ensure consistency.

---

## Strengths

The document does several things well:

1. **Comprehensive Analysis:** The CPU breakdown and root cause analysis clearly justify the optimization.

2. **Phased Approach:** Separating into Phase 2A (simple values) and Phase 2B (object pointers) reduces risk.

3. **Thread Safety Analysis:** Good understanding of dhandle locking and when exclusive access is held.

4. **String Ownership Decision:** The decision to always copy strings to avoid dual-ownership is pragmatic and eliminates a class of bugs.

5. **Invalidation Strategy:** Using `__conn_dhandle_config_clear` as the hook point is correct and leverages existing infrastructure.

6. **Review Feedback Tracking:** Appendix C provides excellent traceability for how feedback was incorporated.

7. **Test Coverage:** The testing strategy is thorough with correctness, concurrency, error recovery, and performance tests.

---

## Recommendations

Prioritized next steps:

### High Priority
1. Fix the `__wt_collator_config` call signature (Critical #1)
2. Create or adapt the encryptor configuration function (Critical #2)
3. Fix union member access in cursor config parsing (Critical #3)

### Medium Priority
4. Decide on `app_metadata` handling for collators
5. Add explicit list of values that must NOT be cached (computed values)
6. Review error cleanup paths

### Low Priority
7. Verify statistics naming against existing patterns
8. Add type definition references
9. Ensure all cache fields are populated in implementation

---

## Verification Checklist

Before concluding this review, I verified:

- [x] Read `bt_handle.c` - verified `__btree_conf` signature and config access
- [x] Read `btree.h` - verified field types (`collator_owned` is `int`, not `bool`)
- [x] Read `dhandle.h` - verified structure (no existing cache field)
- [x] Read `cur_std.c` - verified cursor cache and `have_config` logic
- [x] Read `conn_dhandle.c` - verified `__conn_dhandle_config_clear` location
- [x] Verified `__wt_collator_config` signature in `conn_api.c`
- [x] Verified `__wt_compressor_config` signature in `extern.h`
- [x] Verified `__wt_hash_city64` exists (64-bit, not 32-bit)
- [x] Verified `WT_OPEN_CONFIG_ARG` pattern in `wiredtiger.h.in`
- [x] Checked statistics naming conventions

---

**Review Conclusion:** The design is fundamentally sound but contains several compile-blocking issues that must be fixed. Once the critical issues are addressed, this should be a solid performance improvement.
