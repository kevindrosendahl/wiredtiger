# Unified Structured Configuration System - Design Document

## Executive Summary

This document describes a unified infrastructure for bypassing string-based configuration parsing in two hot paths identified through MongoDB profiling:

1. **Btree Metadata** (~8% CPU) - Parse table metadata string once, cache structured values
2. **Cursor Configuration** (~5% CPU) - New API accepting structured config, bypass parsing entirely

Combined, these optimizations target the `__config_next` function which currently consumes ~14.92% of CPU time even in "warm" scenarios.

## Revision History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | Jan 2026 | Initial design |
| 1.1 | Jan 2026 | Address review feedback: thread safety, string ownership, phased implementation |
| 1.2 | Jan 2026 | Address review feedback: naming consistency, invalidation location, type system, helper signatures |
| 1.3 | Feb 2026 | Address review feedback: function signatures, type accuracy, missing fields, hash functions |
| 1.4 | Feb 2026 | Address review feedback: collator_owned type, compressor/encryptor handling, error cleanup, derived values |
| 1.5 | Feb 2026 | Address review feedback: collator signature, encryptor function, union access, app_metadata |
| 1.6 | Feb 2026 | Address review feedback: storage_tier type, maxmempage runtime-adjusted, btree type derivation |

---

## Table of Contents

1. [Problem Analysis](#1-problem-analysis)
2. [Architecture Overview](#2-architecture-overview)
3. [Part A: Btree Configuration Cache](#3-part-a-btree-configuration-cache)
4. [Part B: Cursor Configuration API](#4-part-b-cursor-configuration-api)
5. [Part C: Fast Path for NULL Config](#5-part-c-fast-path-for-null-config)
6. [Auto-Generation Strategy](#6-auto-generation-strategy)
7. [Testing Strategy](#7-testing-strategy)
8. [Implementation Phases](#8-implementation-phases)
9. [Statistics and Monitoring](#9-statistics-and-monitoring)
10. [Risks and Mitigations](#10-risks-and-mitigations)
11. [Success Metrics](#11-success-metrics)

---

## 1. Problem Analysis

### 1.1 Current State

MongoDB profiling shows `__config_next` at 14.92% CPU even in warm scenarios:

| Source | Approx CPU % | Description |
|--------|-------------|-------------|
| Opening existing tables (dhandle) | ~8% | Parsing table metadata on each session's first access |
| Cursor operations | ~5% | Parsing cursor config on every `open_cursor()` |
| Metadata operations | ~2% | Internal WiredTiger operations |

### 1.2 Root Causes

#### 1.2.1 Btree Metadata Parsing (bt_handle.c)

Every time a session opens a table, WiredTiger parses ~41 config options from the metadata string:

```c
// Called every time ANY session opens a table handle
__wt_btree_open(session, ...)
  → __btree_conf(session, ckpt, is_ckpt)
    // Config accessed via: cfg = btree->dhandle->cfg
    → __wt_config_gets(session, cfg, "allocation_size", &cval);  // Linear scan #1
    → __wt_config_gets(session, cfg, "leaf_page_max", &cval);    // Linear scan #2
    → ... // 39 more linear scans through the same string
```

The metadata string looks like:
```
"key_format=q,value_format=u,allocation_size=4096,internal_page_max=4096,leaf_page_max=32768,..."
```

Each `__wt_config_gets()` performs a linear scan through this ~500+ character string.

#### 1.2.2 Cursor Configuration Parsing (cur_std.c)

Every `session->open_cursor()` call parses ~16 config options:

```c
session->open_cursor(session, "table:test", NULL, "overwrite=false", &cursor);
// Triggers:
//   __cursors_can_be_cached()  - parses 10 options to check cacheability
//   __cursor_reuse_or_init()   - parses 6 options to set cursor flags
```

Even `open_cursor(session, uri, NULL, NULL, &cursor)` triggers all these checks.

### 1.3 Why Existing Caches Don't Help

- **Dhandle cache**: Per-session, so each session re-parses on first access
- **Cursor cache**: Lookup requires parsing config to check cacheability
- **Config is immutable**: Table metadata doesn't change, yet we re-parse every open

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         Unified Config Infrastructure                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                    Shared Type System                                │   │
│  │  - WT_CONF_TYPE enum (bool, int, string)                            │   │
│  │  - WT_CONF_VALUE union                                              │   │
│  │  - Type validation helpers                                          │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                              │                                              │
│              ┌───────────────┴───────────────┐                             │
│              ▼                               ▼                              │
│  ┌─────────────────────────┐    ┌─────────────────────────┐                │
│  │   Btree Config Cache    │    │   Cursor Config API     │                │
│  │   (Part A)              │    │   (Part B)              │                │
│  │                         │    │                         │                │
│  │  - Stored on WT_DHANDLE │    │  - open_cursor_ex()     │                │
│  │  - Parsed once from     │    │  - Structured input     │                │
│  │    metadata string      │    │  - Zero string parsing  │                │
│  │  - ~40 typed fields     │    │  - ~15 typed fields     │                │
│  │  - Shared across        │    │  - Per-cursor config    │                │
│  │    all sessions         │    │                         │                │
│  └─────────────────────────┘    └─────────────────────────┘                │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                    Fast Path Optimizations (Part C)                  │   │
│  │  - Early return for NULL config                                     │   │
│  │  - Special case for "overwrite=false"                               │   │
│  │  - Skip parsing when defaults suffice                               │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Part A: Btree Configuration Cache

### 3.1 Design Goal

Parse table metadata string **once per dhandle lifetime**, cache the parsed values, and reuse them for all subsequent opens by any session.

### 3.2 Data Flow Comparison

#### Current Flow (Slow)

```
Session A opens table "test":
  → Read metadata string from WiredTiger.wt
  → Parse 41 options via __wt_config_gets() (41 linear scans)
  → Store values in WT_BTREE

Session B opens table "test":
  → Read metadata string from WiredTiger.wt
  → Parse 41 options via __wt_config_gets() (41 linear scans)  ← REDUNDANT
  → Store values in WT_BTREE

Session A reopens table "test":
  → Read metadata string from WiredTiger.wt
  → Parse 41 options via __wt_config_gets() (41 linear scans)  ← REDUNDANT
  → Store values in WT_BTREE
```

#### New Flow (Fast)

```
First open of table "test" (any session):
  → Read metadata string from WiredTiger.wt
  → Parse 41 options via __wt_config_gets() (41 linear scans)
  → Cache parsed values on WT_DATA_HANDLE
  → Copy to WT_BTREE

All subsequent opens (any session):
  → Copy cached values from WT_DATA_HANDLE to WT_BTREE  ← O(1)
```

### 3.3 Data Structures

#### 3.3.1 Cache Structure

```c
/*
 * WT_BTREE_CONF --
 *     Cached btree configuration. Stored on WT_DATA_HANDLE, shared across
 *     all sessions. Populated once from metadata string, never re-parsed
 *     until table is altered or dropped.
 */
struct __wt_btree_conf {
    /* Validation flag - set true after successful parse */
    bool parsed;
    
    /*
     * String values (allocated copies, owned by this struct).
     * These are duplicated because the metadata string may be freed/reallocated.
     */
    char *key_format;
    char *value_format;
    char *collator_name;      /* Collator name string, not object pointer */
    char *compressor_name;    /* Compressor name string, not object pointer */
    char *encryption_name;
    char *encryption_keyid;
    
    /*
     * Integer values - TYPES MUST MATCH WT_BTREE exactly to avoid
     * truncation/sign-extension bugs during copy.
     * See src/include/btree.h for authoritative types.
     */
    uint32_t id;
    uint32_t allocsize;
    uint32_t maxintlpage;
    uint32_t maxleafpage;
    uint32_t maxmempage_image;
    /* NOTE: maxmempage is NOT cached - it's runtime-adjusted, see comments below */
    /*
     * IMPORTANT: Several values are NOT cached because they are computed
     * at runtime by __btree_page_sizes() and compression adjustment code.
     * These MUST still be computed after loading from cache:
     *
     * NOT CACHED (runtime-adjusted in __btree_page_sizes, bt_handle.c):
     *   - maxmempage        (lines 1194-1206: RUNTIME ADJUSTED based on 
     *                        cache_size and eviction_dirty_trigger!)
     *   - maxleafkey        (line 1271-1272: derived from leaf_split_size)
     *   - maxleafvalue      (line 1273-1274: derived from leaf_split_size)
     *   - splitmempage      (line 1211: computed from maxmempage)
     *
     * NOTE ON maxmempage: The config value "memory_page_max" is adjusted
     * at runtime based on:
     *   - conn->cache_size (current connection cache size)
     *   - conn->evict->eviction_dirty_trigger (current eviction settings)
     * Formula: MIN(config_value, ((dirty_trigger * cache_size) / 100) / 10)
     * This ensures pages don't grow too large relative to available cache.
     * CACHING THE RAW CONFIG VALUE WOULD SKIP THIS CRITICAL ADJUSTMENT.
     *
     * NOT CACHED (computed based on compressor, bt_handle.c lines 699-720):
     *   - maxintlpage_precomp   (line 700, 714)
     *   - maxleafpage_precomp   (line 702, 718)
     *   - intlpage_compadjust   (line 699, 713)
     *   - leafpage_compadjust   (line 701, 717)
     *
     * After __btree_conf_from_cache(), __btree_page_sizes() MUST still run
     * to compute these values.
     */
    int split_pct;              /* btree.h line 159: int split_pct */
    u_int split_deepen_min_child; /* btree.h line 156: u_int */
    u_int split_deepen_per_child; /* btree.h line 158: u_int */
    u_int dictionary;           /* btree.h line 146: u_int */
    u_int prefix_compression_min; /* btree.h line 149: u_int */
    uint8_t bitcnt;             /* btree.h line 125: fixed-length column store field size */
    
    /*
     * Checksum mode enum - uses existing WT_BTREE_CHECKSUM from btree.h lines 84-89:
     *   typedef enum { CKSUM_ON=1, CKSUM_OFF=2, CKSUM_UNCOMPRESSED=3, CKSUM_UNENCRYPTED=4 }
     */
    WT_BTREE_CHECKSUM checksum;
    
    /* Storage tier for disaggregated storage - use actual enum type */
    WT_BTREE_STORAGE_TIER storage_tier;  /* btree.h line 298: must match actual type */
    
    /* Internal key truncation (row-store specific) */
    bool internal_key_truncate; /* btree.h line 147 */
    
    /*
     * Boolean flags packed into bitfield for cache efficiency.
     * Total: ~15 boolean options = 2 bytes.
     */
    uint32_t flags;
#define WT_BTREE_CONF_CACHE_RESIDENT        0x00001u
#define WT_BTREE_CONF_IGNORE_CACHE_SIZE     0x00002u
#define WT_BTREE_CONF_IN_MEMORY             0x00004u
#define WT_BTREE_CONF_LOG_ENABLED           0x00008u
#define WT_BTREE_CONF_INTERNAL_KEY_TRUNCATE 0x00010u
#define WT_BTREE_CONF_PREFIX_COMPRESSION    0x00020u
#define WT_BTREE_CONF_READONLY              0x00040u
#define WT_BTREE_CONF_TIERED_OBJECT         0x00080u
#define WT_BTREE_CONF_HUFFMAN_VALUE         0x00100u
    /* Additional flags as needed */
    
    /* Tiered storage timestamps */
    uint64_t flush_most_recent_secs;
    uint64_t flush_most_recent_ts;    /* Match btree.h type: uint64_t, not wt_timestamp_t */
};
```

#### 3.3.2 Storage Location

```c
struct __wt_data_handle {
    /* ... existing fields ... */
    
    /*
     * Cached btree configuration. Populated on first open from metadata
     * string, used by all subsequent opens. Protected by dhandle rwlock
     * (write lock needed to populate, read lock sufficient to use).
     *
     * Lifetime: Allocated on first btree open, freed on dhandle close or
     * invalidated on table alter.
     */
    WT_BTREE_CONF *btree_conf_cache;
};
```

### 3.4 Implementation

#### 3.4.1 Cache Population (First Open)

```c
/*
 * __btree_conf_cache_populate --
 *     Parse btree configuration from metadata string and cache it on the dhandle.
 *     Called once per dhandle lifetime under exclusive lock.
 *
 * @param session  Session handle
 * @param dhandle  Data handle to cache config on
 * @param cfg      Configuration string array from metadata
 * @return         0 on success, error code on failure
 */
static int
__btree_conf_cache_populate(
    WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle, const char *cfg[])
{
    WT_BTREE_CONF *conf;
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    
    /* Should only be called when cache doesn't exist or is invalid */
    WT_ASSERT(session, 
        dhandle->btree_conf_cache == NULL || !dhandle->btree_conf_cache->parsed);
    
    /* Allocate cache structure */
    WT_RET(__wt_calloc_one(session, &conf));
    
    /*
     * Parse all configuration options. On any failure, clean up and return.
     * The order matches __btree_conf() for consistency.
     *
     * IMPORTANT: Config keys are either REQUIRED or OPTIONAL:
     * - REQUIRED keys: Use WT_ERR(__wt_config_gets(...)) - fails if not found
     * - OPTIONAL keys: Use WT_ERR_NOTFOUND_OK pattern (see flush_time example)
     *
     * See bt_handle.c lines 639-642 for optional key pattern:
     *   ret = __wt_config_gets(session, cfg, "flush_time", &cval);
     *   WT_RET_NOTFOUND_OK(ret);
     *   if (ret == 0) btree->flush_most_recent_secs = (uint64_t)cval.val;
     */
    
    /* REQUIRED string options - must duplicate since metadata may be freed */
    WT_ERR(__wt_config_gets(session, cfg, "key_format", &cval));
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &conf->key_format));
    
    WT_ERR(__wt_config_gets(session, cfg, "value_format", &cval));
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &conf->value_format));
    
    /* REQUIRED integer options */
    WT_ERR(__wt_config_gets(session, cfg, "id", &cval));
    conf->id = (uint32_t)cval.val;
    
    WT_ERR(__wt_config_gets(session, cfg, "allocation_size", &cval));
    conf->allocsize = (uint32_t)cval.val;
    
    WT_ERR(__wt_config_gets(session, cfg, "internal_page_max", &cval));
    conf->maxintlpage = (uint32_t)cval.val;
    
    WT_ERR(__wt_config_gets(session, cfg, "leaf_page_max", &cval));
    conf->maxleafpage = (uint32_t)cval.val;
    
    /* ... parse remaining ~35 options ... */
    
    /* Boolean flags */
    WT_ERR(__wt_config_gets(session, cfg, "cache_resident", &cval));
    if (cval.val)
        FLD_SET(conf->flags, WT_BTREE_CONF_CACHE_RESIDENT);
    
    WT_ERR(__wt_config_gets(session, cfg, "prefix_compression", &cval));
    if (cval.val)
        FLD_SET(conf->flags, WT_BTREE_CONF_PREFIX_COMPRESSION);
    
    /* ... parse remaining boolean options ... */
    
    /* Mark as successfully parsed and install on dhandle */
    conf->parsed = true;
    
    /* Free any existing invalid cache before installing new one */
    if (dhandle->btree_conf_cache != NULL)
        __btree_conf_cache_free(session, dhandle->btree_conf_cache);
    
    dhandle->btree_conf_cache = conf;
    return (0);
    
err:
    __btree_conf_cache_free(session, conf);
    return (ret);
}
```

#### 3.4.2 Cache Usage (Subsequent Opens)

**IMPORTANT: String Ownership Design Decision**

The existing `__btree_clear()` function frees btree-owned strings:

```c
// In bt_handle.c __btree_clear():
__wt_free(session, btree->key_format);
__wt_free(session, btree->value_format);
```

To avoid dual-ownership complexity and potential double-free bugs, we **always copy strings** from cache to btree. This adds ~100 bytes memory per btree open but eliminates ownership tracking complexity.

```c
/*
 * __btree_conf_from_cache --
 *     Apply cached configuration to btree handle. This is an O(n) operation
 *     where n is the number of string fields (typically 2-6), replacing
 *     ~41 config string parses.
 *
 * @param session  Session handle
 * @param btree    Btree handle to configure
 * @param conf     Cached configuration
 * @return         0 on success, error code on failure
 */
static int
__btree_conf_from_cache(WT_SESSION_IMPL *session, WT_BTREE *btree, WT_BTREE_CONF *conf)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    
    WT_ASSERT(session, conf != NULL && conf->parsed);
    
    /*
     * String values - ALWAYS COPY to btree.
     * 
     * Design rationale: __btree_clear() frees btree->key_format etc.
     * If we shared pointers with the cache, we'd have dual-ownership
     * and risk double-free. Copying is simpler and safer.
     * 
     * Cost: ~100 bytes extra memory per btree open (negligible).
     */
    WT_ERR(__wt_strdup(session, conf->key_format, &btree->key_format));
    WT_ERR(__wt_strdup(session, conf->value_format, &btree->value_format));
    
    /*
     * NOTE: collator_name is NOT copied to btree.
     * The btree structure has btree->collator (object pointer) and 
     * btree->collator_owned, NOT a string name. The cached name is used
     * only for the collator lookup below.
     */
    
    /* Integer values - direct copy */
    btree->id = conf->id;
    btree->allocsize = conf->allocsize;
    btree->maxintlpage = conf->maxintlpage;
    btree->maxleafpage = conf->maxleafpage;
    btree->maxmempage_image = conf->maxmempage_image;
    /* 
     * NOTE: maxmempage is NOT cached - it's runtime-adjusted based on
     * cache_size and eviction_dirty_trigger. See bt_handle.c lines 1194-1206.
     * It will be set correctly when __btree_page_sizes() runs after this.
     *
     * NOTE: maxleafkey/maxleafvalue are NOT cached - derived in __btree_page_sizes()
     */
    btree->split_pct = conf->split_pct;
    btree->split_deepen_min_child = conf->split_deepen_min_child;
    btree->split_deepen_per_child = conf->split_deepen_per_child;
    btree->dictionary = conf->dictionary;
    btree->prefix_compression_min = conf->prefix_compression_min;
    btree->checksum = conf->checksum;
    btree->bitcnt = conf->bitcnt;  /* Fixed-length column store field size */
    
    /* Boolean flags - apply to btree flags */
    if (FLD_ISSET(conf->flags, WT_BTREE_CONF_CACHE_RESIDENT))
        F_SET(btree, WT_BTREE_NO_EVICT);
    if (FLD_ISSET(conf->flags, WT_BTREE_CONF_IN_MEMORY))
        F_SET(btree, WT_BTREE_IN_MEMORY);
    if (FLD_ISSET(conf->flags, WT_BTREE_CONF_LOG_ENABLED))
        F_SET(btree, WT_BTREE_LOGGED);
    if (FLD_ISSET(conf->flags, WT_BTREE_CONF_PREFIX_COMPRESSION))
        btree->prefix_compression = true;
    if (FLD_ISSET(conf->flags, WT_BTREE_CONF_INTERNAL_KEY_TRUNCATE))
        btree->internal_key_truncate = true;
    if (FLD_ISSET(conf->flags, WT_BTREE_CONF_READONLY))
        F_SET(btree, WT_BTREE_READONLY);
    
    /*
     * Compressor lookup - REQUIRED even with cache.
     * Uses cached name string to lookup compressor object.
     * See bt_handle.c lines 690-691.
     */
    if (conf->compressor_name != NULL && conf->compressor_name[0] != '\0') {
        cval.str = conf->compressor_name;
        cval.len = strlen(conf->compressor_name);
        WT_ERR(__wt_compressor_config(session, &cval, &btree->compressor));
    }
    
    /*
     * Encryptor lookup - REQUIRED even with cache.
     * 
     * IMPORTANT: __wt_btree_config_encryptor takes const char **cfg,
     * NOT individual strings. We must still read from dhandle->cfg
     * for encryption config. This is acceptable since encryption
     * config is typically static per-table.
     *
     * See bt_handle.c line 723:
     *   WT_RET(__wt_btree_config_encryptor(session, cfg, &btree->kencryptor));
     */
    WT_ERR(__wt_btree_config_encryptor(session, btree->dhandle->cfg, &btree->kencryptor));
    
    /*
     * Collator lookup - handled separately if collator_name is set.
     *
     * IMPORTANT: __wt_collator_config requires BOTH collator name AND
     * app_metadata parameters. See bt_handle.c lines 526-530:
     *   WT_RET(__wt_config_gets(session, cfg, "app_metadata", &metadata));
     *   WT_RET(__wt_collator_config(session, btree->dhandle->name, &cval, 
     *          &metadata, &btree->collator, &btree->collator_owned));
     *
     * For Phase 2A, we read app_metadata from dhandle->cfg. The collator
     * name is cached for fast path detection (skip if empty).
     */
    if (conf->collator_name != NULL && conf->collator_name[0] != '\0') {
        WT_CONFIG_ITEM metadata;
        
        cval.str = conf->collator_name;
        cval.len = strlen(conf->collator_name);
        
        /* Must still read app_metadata from config for collator init */
        WT_ERR(__wt_config_gets(session, btree->dhandle->cfg, "app_metadata", &metadata));
        WT_ERR(__wt_collator_config(session, btree->dhandle->name, &cval, 
            &metadata, &btree->collator, &btree->collator_owned));
    }
    
    return (0);
    
err:
    /*
     * Clean up any partially-allocated resources on error.
     *
     * Only key_format and value_format are string copies to btree.
     * Collator/compressor are object pointers looked up, not allocated here.
     *
     * NOTE: On error, the caller (__btree_conf) should still follow normal
     * error paths. The actual __btree_conf relies on __btree_clear() for
     * full cleanup. Setting these to NULL ensures __btree_clear() won't
     * double-free if the strings weren't yet allocated.
     */
    __wt_free(session, btree->key_format);
    __wt_free(session, btree->value_format);
    btree->key_format = NULL;
    btree->value_format = NULL;
    return (ret);
}
```

#### 3.4.3 Modified __btree_conf Flow

**IMPORTANT**: The actual `__btree_conf` signature differs from earlier document versions:

```c
// Actual signature (bt_handle.c line 483-484):
static int __btree_conf(WT_SESSION_IMPL *session, WT_CKPT *ckpt, bool is_ckpt)

// Config is accessed via: cfg = btree->dhandle->cfg (line 494)
// NOT passed as a function parameter
```

```c
/*
 * __btree_conf --
 *     Configure a btree handle. Uses cached config if available,
 *     otherwise parses from string and caches for future use.
 *
 * NOTE: ckpt and is_ckpt parameters are used for write generation
 * calculation which happens AFTER all config is applied.
 */
static int
__btree_conf(WT_SESSION_IMPL *session, WT_CKPT *ckpt, bool is_ckpt)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    const char **cfg;
    
    btree = S2BT(session);
    conn = S2C(session);
    dhandle = session->dhandle;
    cfg = btree->dhandle->cfg;  /* Config accessed via dhandle */
    
    /*
     * Apply config from cache or parse from string.
     */
    if (dhandle->btree_conf_cache != NULL && dhandle->btree_conf_cache->parsed) {
        WT_STAT_CONN_INCR(session, btree_conf_cache_hit);
        WT_RET(__btree_conf_from_cache(session, btree, dhandle->btree_conf_cache));
    } else {
        WT_STAT_CONN_INCR(session, btree_conf_cache_miss);
        WT_RET(__btree_conf_cache_populate(session, dhandle, cfg));
        WT_RET(__btree_conf_from_cache(session, btree, dhandle->btree_conf_cache));
    }
    
    /*
     * Derive btree->type from key_format. This is a simple derivation
     * that we do here rather than caching, matching bt_handle.c lines 512-517.
     */
    if (btree->key_format[0] == 'r')
        btree->type = BTREE_COL_VAR;
    else
        btree->type = BTREE_ROW;
    
    /*
     * CRITICAL: Page sizes MUST be computed after loading from cache.
     * This handles runtime-adjusted values including:
     *   - maxmempage (adjusted based on cache_size/dirty_trigger)
     *   - splitmempage (derived from maxmempage)
     *   - maxleafkey/maxleafvalue (derived from split calculations)
     *   - compression adjustment values
     * See bt_handle.c line 635.
     */
    WT_RET(__btree_page_sizes(session));
    
    /*
     * Write generation logic (existing code path, not changed).
     * This is the checkpoint-specific part that uses ckpt/is_ckpt.
     * See bt_handle.c lines 763-791.
     */
    btree->write_gen = WT_MAX(ckpt->write_gen + 1, conn->base_write_gen);
    /* ... rest of write generation code unchanged ... */
    
    return (0);
}
```

### 3.5 Phased Implementation for Complex Fields

**Problem**: Some btree configuration fields are not simple values but loaded objects:

| Field | Type | Complexity |
|-------|------|------------|
| `collator` | `WT_COLLATOR *` | Requires object lookup |
| `compressor` | `WT_COMPRESSOR *` | Requires object lookup |
| `kencryptor` | `WT_KEYED_ENCRYPTOR *` | Complex encryption config |

**Solution**: Implement btree config caching in phases:

#### Phase 2A: Simple Values Only (Initial Implementation)

Cache only integer and boolean values that don't require object lookups:

```c
struct __wt_btree_conf {
    bool parsed;
    
    /* Phase 2A: Simple values only */
    uint32_t id;
    uint32_t allocsize;
    uint32_t maxintlpage;
    uint32_t maxleafpage;
    /* ... other integers ... */
    
    uint32_t flags;  /* Boolean flags */
    
    /* String VALUES cached, but collator/compressor objects still
     * need to be looked up from these strings on each open */
    char *key_format;
    char *value_format;
    char *collator_name;      /* String only, not WT_COLLATOR* */
    char *compressor_name;    /* String only, not WT_COMPRESSOR* */
    char *encryption_name;    /* String only */
    char *encryption_keyid;   /* String only */
};
```

In `__btree_conf_from_cache()`, we still call the object lookup functions:

```c
/* Phase 2A: Object lookups still required, but string parsing avoided */
if (conf->collator_name != NULL && conf->collator_name[0] != '\0') {
    /* Lookup collator object from cached name - this is O(hash lookup) */
    WT_RET(__wt_collator_config(session, btree, conf->collator_name, ...));
}
if (conf->compressor_name != NULL && conf->compressor_name[0] != '\0') {
    WT_RET(__wt_compressor_config(session, conf->compressor_name, ...));
}
```

**Benefit**: Eliminates ~35 string parses, keeps only ~3 object lookups.

#### Phase 2B: Object Pointer Caching (Future Optimization)

Once Phase 2A is proven stable, consider caching object pointers:

```c
struct __wt_btree_conf {
    /* ... Phase 2A fields ... */
    
    /* Phase 2B: Cached object pointers (optional future work) */
    WT_COLLATOR *collator;       /* Cached pointer */
    WT_COMPRESSOR *compressor;   /* Cached pointer */
    int collator_owned;          /* btree.h line 128: int, not bool */
};
```

**Prerequisite**: Must verify that collator/compressor objects outlive dhandle, or add reference counting.

**NOTE on `collator_owned`**: The field is `int` in btree.h (line 128), not `bool`. This is used with `WT_COLLATOR.terminate` callback logic. Using the wrong type could cause subtle bugs.
```

**Prerequisite**: Must verify that collator/compressor objects outlive dhandle, or add reference counting.

### 3.6 Cache Invalidation

The cache must be invalidated when table configuration changes:

```c
/*
 * __wt_btree_conf_cache_invalidate --
 *     Invalidate the btree configuration cache. Called when table metadata
 *     changes (alter) or when dhandle is being destroyed.
 *
 * @param session  Session handle
 * @param dhandle  Data handle whose cache should be invalidated
 */
void
__wt_btree_conf_cache_invalidate(WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle)
{
    if (dhandle->btree_conf_cache != NULL) {
        __btree_conf_cache_free(session, dhandle->btree_conf_cache);
        dhandle->btree_conf_cache = NULL;
    }
}

/*
 * __btree_conf_cache_free --
 *     Free a btree configuration cache structure and all owned memory.
 */
static void
__btree_conf_cache_free(WT_SESSION_IMPL *session, WT_BTREE_CONF *conf)
{
    if (conf == NULL)
        return;
    
    /* Free owned string copies */
    __wt_free(session, conf->key_format);
    __wt_free(session, conf->value_format);
    __wt_free(session, conf->collator_name);
    __wt_free(session, conf->compressor_name);
    __wt_free(session, conf->encryption_name);
    __wt_free(session, conf->encryption_keyid);
    
    __wt_free(session, conf);
}
```

#### Invalidation Points

| Operation | Action | Location |
|-----------|--------|----------|
| `session->alter()` | Invalidate cache | `schema_alter.c` |
| `session->drop()` | Free cache | `conn_dhandle.c` |
| Dhandle close | Free cache | `conn_dhandle.c` |
| Dhandle eviction | Free cache | `conn_dhandle.c` |

#### Cache Invalidation Location

The cache should be invalidated in `__conn_dhandle_config_clear()`, which is called:
- Before reopening a dhandle (line 616 in conn_dhandle.c)
- In `__conn_dhandle_destroy()` (line 180 in conn_dhandle.c)

This leverages the existing config clearing mechanism:

```c
// In src/conn/conn_dhandle.c, __conn_dhandle_config_clear():
static void
__conn_dhandle_config_clear(WT_SESSION_IMPL *session)
{
    WT_DATA_HANDLE *dhandle;
    const char **a;

    dhandle = session->dhandle;

    /*
     * Invalidate btree config cache when config is cleared.
     * This handles both alter (reopen) and destroy paths.
     */
    if (dhandle->btree_conf_cache != NULL) {
        __btree_conf_cache_free(session, dhandle->btree_conf_cache);
        dhandle->btree_conf_cache = NULL;
    }

    if (dhandle->cfg == NULL)
        return;
    for (a = dhandle->cfg; *a != NULL; ++a)
        __wt_free(session, *a);
    __wt_free(session, dhandle->cfg);
    dhandle->cfg = NULL;
}
```

**Call sites in conn_dhandle.c:**
- Line 180: Called from `__conn_dhandle_destroy()` during dhandle cleanup
- Line 616: Called from `__wt_conn_dhandle_open()` when reopening with new config

### 3.6 Thread Safety

#### Analysis

The `__btree_conf()` function is called from `__wt_btree_open()`, which is called from `__wt_conn_dhandle_open()`. At this point, the dhandle is held with `WT_DHANDLE_EXCLUSIVE` lock:

```c
// In __wt_session_get_dhandle():
WT_ASSERT(session, F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE));
if ((ret = __wt_conn_dhandle_open(session, cfg, flags)) == 0 ...)
```

This means cache population is inherently serialized by the existing exclusive lock.

#### Defensive Double-Check Pattern

Despite the exclusive lock guarantee, we add a defensive double-check pattern for safety:

```c
static int
__btree_conf(WT_SESSION_IMPL *session, WT_CKPT *ckpt, bool is_ckpt)
{
    WT_BTREE *btree = S2BT(session);
    WT_DATA_HANDLE *dhandle = session->dhandle;
    const char **cfg = btree->dhandle->cfg;  /* Config via dhandle */
    
    /*
     * Fast path: use cached config if available.
     * Note: We must hold at least a read lock on the dhandle, which is
     * guaranteed by the session dhandle reference.
     */
    if (dhandle->btree_conf_cache != NULL && dhandle->btree_conf_cache->parsed) {
        WT_STAT_CONN_INCR(session, btree_conf_cache_hit);
        return (__btree_conf_from_cache(session, btree, dhandle->btree_conf_cache));
    }
    
    /*
     * Slow path: populate cache. We should hold exclusive lock here
     * (asserted below), but double-check the cache wasn't populated
     * by another thread as a defensive measure.
     */
    WT_ASSERT(session, F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE));
    
    /* Double-check after acquiring exclusive access */
    if (dhandle->btree_conf_cache != NULL && dhandle->btree_conf_cache->parsed) {
        WT_STAT_CONN_INCR(session, btree_conf_cache_hit);
        return (__btree_conf_from_cache(session, btree, dhandle->btree_conf_cache));
    }
    
    WT_STAT_CONN_INCR(session, btree_conf_cache_miss);
    WT_RET(__btree_conf_cache_populate(session, dhandle, cfg));
    return (__btree_conf_from_cache(session, btree, dhandle->btree_conf_cache));
}
```

#### Lock Requirements Summary

| Operation | Required Lock | How Acquired |
|-----------|--------------|--------------|
| Cache population | Dhandle exclusive | `__wt_conn_dhandle_open()` |
| Cache read | Dhandle read | Session dhandle reference |
| Cache invalidation | Dhandle exclusive | `session->alter()`, `session->drop()` |

No additional locking is required beyond existing dhandle locks.

### 3.8 Debug Mode Cache Bypass

For debugging cache-related issues, add a diagnostic flag to bypass the cache:

```c
#ifdef HAVE_DIAGNOSTIC
/*
 * __btree_conf --
 *     Configure a btree handle.
 *
 * Actual signature: (session, ckpt, is_ckpt)
 * Config accessed via: btree->dhandle->cfg
 */
static int
__btree_conf(WT_SESSION_IMPL *session, WT_CKPT *ckpt, bool is_ckpt)
{
    WT_CONNECTION_IMPL *conn = S2C(session);
    const char **cfg = S2BT(session)->dhandle->cfg;
    
    /*
     * Debug bypass: Allow disabling cache for troubleshooting.
     * Enable via: debug_mode=(btree_conf_cache=false)
     */
    if (FLD_ISSET(conn->debug_flags, WT_CONN_DEBUG_BTREE_CONF_CACHE_OFF))
        return (__btree_conf_slow_path(session, cfg, ckpt, is_ckpt));
    
    /* Normal cache path ... */
}
#endif
```

This allows isolating cache-related bugs by running with:
```
wiredtiger_open(home, NULL, "create,debug_mode=(btree_conf_cache=false)", &conn);
```

---

## 4. Part B: Cursor Configuration API

### 4.1 Design Goal

Provide a new `open_cursor_ex()` API that accepts structured configuration, completely bypassing string parsing for cursor opens.

### 4.2 Public API

#### 4.2.1 Key Definitions

**Naming Convention**: Align with existing `WT_OPEN_CONFIG_ARG` pattern from `wiredtiger_open_ex()`.

```c
/* src/include/wiredtiger_cursor_conf.h (auto-generated) */

/*
 * Cursor configuration key IDs.
 * These are stable across versions for ABI compatibility.
 *
 * Naming follows existing WT_OPEN_CONF_* pattern.
 */
#define WT_CURSOR_CONF_append           1001  /* bool: append to column store */
#define WT_CURSOR_CONF_bulk             1002  /* bool: bulk load mode */
#define WT_CURSOR_CONF_checkpoint       1003  /* string: checkpoint name */
#define WT_CURSOR_CONF_debug            1004  /* string: debug options */
#define WT_CURSOR_CONF_dump             1005  /* string: dump format */
#define WT_CURSOR_CONF_next_random      1006  /* bool: random iteration */
#define WT_CURSOR_CONF_overwrite        1007  /* bool: overwrite on insert */
#define WT_CURSOR_CONF_raw              1008  /* bool: raw mode */
#define WT_CURSOR_CONF_read_once        1009  /* bool: read-once hint */
#define WT_CURSOR_CONF_readonly         1010  /* bool: read-only cursor */

/*
 * WT_CURSOR_CONFIG_ARG --
 *     Single cursor configuration argument for open_cursor_ex().
 *
 * This structure mirrors WT_OPEN_CONFIG_ARG used by wiredtiger_open_ex()
 * for API consistency.
 */
/*
 * Structure layout must match WT_OPEN_CONFIG_ARG for consistency.
 * See wiredtiger.h.in for the existing pattern.
 */
typedef struct __wt_cursor_config_arg {
    uint64_t key;           /* WT_CURSOR_CONF_* key ID */
    union {
        int64_t v_int;      /* For BOOL (0/1) and INT values */
        struct {
            const char *str;
            size_t len;
        } v_str;
    } value;                /* Named union to match existing pattern */
    uint8_t type;           /* WT_CURSOR_CONFIG_ARG_BOOL/INT/STR */
} WT_CURSOR_CONFIG_ARG;

/*
 * Helper macros for building configuration arrays.
 * Follow EXACT pattern from WT_OPEN_CONFIG_ARG_SET_* macros.
 *
 * From wiredtiger.h.in lines 3627-3628:
 *   #define WT_OPEN_CONFIG_ARG_SET_BOOL(key_id, val) \
 *       { .key = (key_id), .value.v_int = (val) ? 1 : 0, .type = WT_OPEN_CONFIG_ARG_BOOL }
 *
 * NOTE: BOOL uses .value.v_int with explicit 0/1 conversion, NOT .v_bool
 */
#define WT_CURSOR_CONFIG_ARG_SET_BOOL(k, v) \
    { .key = (k), .value.v_int = (v) ? 1 : 0, .type = WT_CURSOR_CONFIG_ARG_BOOL }

#define WT_CURSOR_CONFIG_ARG_SET_INT(k, v) \
    { .key = (k), .value.v_int = (v), .type = WT_CURSOR_CONFIG_ARG_INT }

#define WT_CURSOR_CONFIG_ARG_SET_STR(k, s, l) \
    { .key = (k), .value.v_str = { .str = (s), .len = (l) }, .type = WT_CURSOR_CONFIG_ARG_STR }

#define WT_CURSOR_CONFIG_ARG_END \
    { .key = 0 }

/*
 * Type enum alignment with existing wiredtiger_open_ex() types.
 * 
 * The existing implementation uses:
 *   WT_OPEN_CONFIG_ARG_BOOL = 0
 *   WT_OPEN_CONFIG_ARG_INT  = 1
 *   WT_OPEN_CONFIG_ARG_STR  = 2
 * 
 * For cursor config, we have two options:
 * Option A: Use same enum values with new names:
 *   #define WT_CURSOR_CONFIG_ARG_BOOL  0
 *   #define WT_CURSOR_CONFIG_ARG_INT   1
 *   #define WT_CURSOR_CONFIG_ARG_STR   2
 * 
 * Option B: Define a shared type enum used by both APIs:
 *   typedef enum {
 *       WT_CONFIG_ARG_BOOL = 0,
 *       WT_CONFIG_ARG_INT  = 1,
 *       WT_CONFIG_ARG_STR  = 2
 *   } WT_CONFIG_ARG_TYPE;
 * 
 * Recommendation: Option A for backwards compatibility with existing
 * wiredtiger_open_ex() users. The values are identical so code can
 * share validation logic.
 */
#define WT_CURSOR_CONFIG_ARG_BOOL  0
#define WT_CURSOR_CONFIG_ARG_INT   1
#define WT_CURSOR_CONFIG_ARG_STR   2
```

#### 4.2.2 Session Method

```c
/* In wiredtiger.h, added to WT_SESSION */

/*!
 * Open a cursor with structured configuration.
 *
 * This is a high-performance alternative to WT_SESSION::open_cursor that
 * accepts pre-parsed configuration, avoiding string parsing overhead.
 * Use this method when cursor open performance is critical.
 *
 * @configstart{WT_SESSION.open_cursor_ex, see dist/api_data.py}
 * @config{append, append inserted values as new records (column store only).
 *     Type: bool. Default: false.}
 * @config{bulk, configure for bulk load. Type: bool. Default: false.}
 * @config{checkpoint, open a cursor on a checkpoint. Type: string.}
 * @config{overwrite, overwrite existing values on insert. Type: bool.
 *     Default: true.}
 * @config{raw, ignore key/value encoding. Type: bool. Default: false.}
 * @config{readonly, open cursor in read-only mode. Type: bool. Default: false.}
 * @configend
 *
 * Example:
 * @code
 * WT_CURSOR_CONFIG_ARG config[] = {
 *     WT_CURSOR_CONFIG_ARG_SET_BOOL(WT_CURSOR_CONF_overwrite, false),
 *     WT_CURSOR_CONFIG_ARG_END
 * };
 * error_check(session->open_cursor_ex(session, "table:mytable", config, 0, &cursor));
 * @endcode
 *
 * @param session the session handle
 * @param uri the data source URI
 * @param config configuration array, terminated by WT_CURSOR_CONFIG_ARG_END.
 *     NULL is equivalent to an empty array (all defaults).
 * @param config_count number of elements in config array. If 0, the array
 *     must be terminated by WT_CURSOR_CONFIG_ARG_END.
 * @param[out] cursorp the returned cursor handle
 * @errors
 *
 * NOTE: This is a PUBLIC API method added to WT_SESSION struct vtable,
 * not an internal __wt_ prefixed function. The __wt_ prefix is used for
 * internal helper functions (like __wt_session_open_cursor_int).
 *
 * Public method declaration (added to WT_SESSION struct in wiredtiger.h):
 *   int (*open_cursor_ex)(WT_SESSION *session, const char *uri,
 *       const WT_CURSOR_CONFIG_ARG *config, size_t config_count, 
 *       WT_CURSOR **cursorp);
 *
 * Internal implementation function:
 *   static int __session_open_cursor_ex(...);  // in session_api.c
 */
int (*open_cursor_ex)(WT_SESSION *session, const char *uri,
    const WT_CURSOR_CONFIG_ARG *config, size_t config_count, WT_CURSOR **cursorp);
```

### 4.3 Internal Implementation

#### 4.3.1 Parsed Config Structure

```c
/*
 * WT_CURSOR_CONFIG --
 *     Internal parsed cursor configuration. Used for both struct API
 *     and optimized string parsing paths.
 */
typedef struct __wt_cursor_config {
    /* Computed properties */
    bool cacheable;         /* Can this cursor be cached? */
    uint64_t hash;          /* Hash for cache lookup */
    
    /* Boolean options (with defaults) */
    bool append;            /* Default: false */
    bool bulk;              /* Default: false */
    bool next_random;       /* Default: false */
    bool overwrite;         /* Default: true (!) */
    bool raw;               /* Default: false */
    bool read_once;         /* Default: false */
    bool readonly;          /* Default: false */
    bool force;             /* Default: false */
    
    /* String options (not owned, must outlive cursor) */
    const char *checkpoint;
    size_t checkpoint_len;
    const char *debug;
    size_t debug_len;
    const char *dump;
    size_t dump_len;
} WT_CURSOR_CONFIG;
```

#### 4.3.2 Config Parsing

```c
/*
 * __cursor_config_init_defaults --
 *     Initialize cursor config structure with default values.
 */
static inline void
__cursor_config_init_defaults(WT_CURSOR_CONFIG *config)
{
    memset(config, 0, sizeof(*config));
    config->cacheable = true;
    config->overwrite = true;  /* Note: default is TRUE for overwrite */
}

/*
 * __cursor_config_parse_struct --
 *     Parse structured cursor config into internal format.
 *     O(n) where n is number of provided options (typically 0-3).
 */
static int
__cursor_config_parse_struct(WT_SESSION_IMPL *session,
    const WT_CURSOR_CONFIG_ARG *args, size_t n_args, WT_CURSOR_CONFIG *config)
{
    size_t i;
    
    __cursor_config_init_defaults(config);
    
    /* Fast path: NULL or empty config = all defaults */
    if (args == NULL)
        return (0);
    
    /* Determine count if not provided */
    if (n_args == 0) {
        for (n_args = 0; args[n_args].key != 0; n_args++) {
            if (n_args > 100)  /* Sanity check */
                WT_RET_MSG(session, EINVAL, "Cursor config array too large");
        }
    }
    
    /* Parse each provided option */
    for (i = 0; i < n_args && args[i].key != 0; i++) {
        switch (args[i].key) {
        case WT_CURSOR_CONF_append:
            WT_RET(__cursor_config_validate_type(session, &args[i], WT_CONF_TYPE_BOOL));
            config->append = (args[i].value.v_int != 0);  /* BOOL uses v_int */
            break;
            
        case WT_CURSOR_CONF_bulk:
            WT_RET(__cursor_config_validate_type(session, &args[i], WT_CONF_TYPE_BOOL));
            config->bulk = (args[i].value.v_int != 0);
            if (config->bulk)
                config->cacheable = false;
            break;
            
        case WT_CURSOR_CONF_checkpoint:
            WT_RET(__cursor_config_validate_type(session, &args[i], WT_CONF_TYPE_STRING));
            config->checkpoint = args[i].value.v_str.str;  /* Note: .value prefix */
            config->checkpoint_len = args[i].value.v_str.len;
            if (config->checkpoint_len > 0)
                config->cacheable = false;
            break;
            
        case WT_CURSOR_CONF_overwrite:
            WT_RET(__cursor_config_validate_type(session, &args[i], WT_CONF_TYPE_BOOL));
            config->overwrite = (args[i].value.v_int != 0);
            break;
            
        case WT_CURSOR_CONF_raw:
            WT_RET(__cursor_config_validate_type(session, &args[i], WT_CONF_TYPE_BOOL));
            config->raw = (args[i].value.v_int != 0);
            break;
            
        case WT_CURSOR_CONF_readonly:
            WT_RET(__cursor_config_validate_type(session, &args[i], WT_CONF_TYPE_BOOL));
            config->readonly = (args[i].value.v_int != 0);
            if (config->readonly)
                config->cacheable = false;
            break;
            
        /* ... handle remaining options ... */
            
        default:
            WT_RET_MSG(session, EINVAL,
                "Unknown cursor config key: %" PRIu64, args[i].key);
        }
    }
    
    /* Compute hash for cache lookup */
    config->hash = __cursor_config_hash(config);
    
    return (0);
}

/*
 * __cursor_config_hash --
 *     Compute a hash of cursor configuration for cache lookup.
 *     The hash must be stable and collision-resistant for the fields
 *     that affect cursor behavior.
 *
 * NOTE: WiredTiger provides __wt_hash_city64 (src/include/extern.h line 1665),
 * not __wt_hash_city32. Use 64-bit hash for better distribution.
 *
 * @param config  Parsed configuration structure
 * @return        64-bit hash value
 */
static uint64_t
__cursor_config_hash(const WT_CURSOR_CONFIG *config)
{
    /*
     * Pack all cacheable-affecting boolean flags into a single uint8_t
     * for efficient hashing. This is more efficient than hashing
     * individual boolean fields.
     *
     * FLAGS INCLUDED IN HASH:
     *   - overwrite, raw, readonly
     *
     * FLAGS NOT IN HASH (handled by post-cache patching):
     *   - append: Existing cursor cache clears APPEND on reuse and sets
     *             from new config (cur_std.c line 1091: F_CLR(cursor, WT_CURSTD_APPEND))
     *   - read_once: Affects eviction hints, not cursor behavior matching
     *
     * FLAGS NOT IN HASH (make cursor non-cacheable):
     *   - bulk, debug, dump: These set cacheable=false, so cursor won't
     *     be in cache to begin with
     *
     * This matches the existing cursor cache behavior in __wt_cursor_cache_get()
     * which patches flags after retrieval rather than using them for matching.
     */
    uint8_t flags = 0;
    if (config->overwrite)
        flags |= 0x01;
    if (config->raw)
        flags |= 0x02;
    if (config->readonly)
        flags |= 0x04;
    
    return __wt_hash_city64(&flags, sizeof(flags));
}

/*
 * __cursor_config_matches --
 *     Check if a cached cursor's config hash matches the requested config.
 *
 * @param cached_hash    Hash from cached cursor (64-bit)
 * @param requested_hash Hash from requested config (64-bit)
 * @return               true if configs match
 */
static inline bool
__cursor_config_matches(uint64_t cached_hash, uint64_t requested_hash)
{
    return cached_hash == requested_hash;
}

/*
 * __cursor_config_apply --
 *     Apply configuration to a cursor retrieved from cache.
 *     Most config is already correct from cache; this handles
 *     any runtime state that needs updating.
 *
 * @param cursor  Cursor to configure
 * @param config  Parsed configuration
 */
static void
__cursor_config_apply(WT_CURSOR *cursor, const WT_CURSOR_CONFIG *config)
{
    /* 
     * For cacheable cursors, most state is preserved.
     * Only update flags that might differ:
     */
    if (config->overwrite)
        F_SET(cursor, WT_CURSTD_OVERWRITE);
    else
        F_CLR(cursor, WT_CURSTD_OVERWRITE);
    
    if (config->raw)
        F_SET(cursor, WT_CURSTD_RAW);
    else
        F_CLR(cursor, WT_CURSTD_RAW);
}
```

#### 4.3.3 Main Entry Point

```c
/*
 * __session_open_cursor_ex --
 *     WT_SESSION->open_cursor_ex implementation.
 */
static int
__session_open_cursor_ex(WT_SESSION *wt_session, const char *uri,
    const WT_CURSOR_CONFIG_ARG *config, size_t config_count, WT_CURSOR **cursorp)
{
    WT_CURSOR *cursor;
    WT_CURSOR_CONFIG parsed;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    
    cursor = NULL;
    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL(session, open_cursor_ex, NULL);
    
    WT_ERR(__wt_str_name_check(session, uri));
    
    /* Parse structured config - O(n) where n is small */
    WT_ERR(__cursor_config_parse_struct(session, config, config_count, &parsed));
    
    /* Try cursor cache if applicable */
    if (parsed.cacheable && F_ISSET(session, WT_SESSION_CACHE_CURSORS)) {
        ret = __cursor_cache_get_ex(session, uri, &parsed, &cursor);
        if (ret == 0)
            goto done;
        WT_ERR_NOTFOUND_OK(ret, false);
    }
    
    /* Open new cursor with parsed config */
    WT_ERR(__session_open_cursor_int_ex(session, uri, &parsed, &cursor));
    
done:
    *cursorp = cursor;
    
err:
    API_END_RET(session, ret);
}
```

### 4.4 Cursor Cache Integration

The cursor cache can be extended to work with parsed config:

```c
/*
 * __cursor_cache_get_ex --
 *     Get a cursor from the cache using parsed config for matching.
 */
static int
__cursor_cache_get_ex(WT_SESSION_IMPL *session, const char *uri,
    WT_CURSOR_CONFIG *config, WT_CURSOR **cursorp)
{
    WT_CURSOR *cursor;
    WT_CURSOR_BTREE *cbt;
    uint64_t bucket;
    
    /*
     * Bucket computed from hash_value parameter (passed by caller).
     * Matches actual code in cur_std.c line 1074:
     *   bucket = hash_value & (S2C(session)->hash_size - 1);
     * The hash_value is typically computed by caller as:
     *   hash_value = __wt_hash_city64(uri, strlen(uri));
     */
    bucket = hash_value & (S2C(session)->hash_size - 1);
    
    /* Search cache for matching cursor */
    TAILQ_FOREACH(cursor, &session->cursor_cache[bucket], cache_q) {
        if (cursor->uri_hash != hash_value || strcmp(cursor->uri, uri) != 0)
            continue;
            
        cbt = (WT_CURSOR_BTREE *)cursor;
        
        /* Check if cached cursor config matches requested config */
        if (!__cursor_config_matches(cbt->cached_config_hash, config->hash))
            continue;
        
        /* Found a match - remove from cache and return */
        TAILQ_REMOVE(&session->cursor_cache[bucket], cursor, cache_q);
        session->cursor_cache_count--;
        
        /* Apply any config that differs from cached state */
        __cursor_config_apply(cursor, config);
        
        WT_STAT_CONN_INCR(session, cursor_cache_hit);
        *cursorp = cursor;
        return (0);
    }
    
    WT_STAT_CONN_INCR(session, cursor_cache_miss);
    return (WT_NOTFOUND);
}
```

---

## 5. Part C: Fast Path for NULL Config

### 5.1 Design Goal

Optimize the existing `open_cursor()` API for the common case of NULL or minimal configuration.

### 5.2 Implementation

#### 5.2.1 Cacheability Check

```c
/*
 * __cursors_can_be_cached --
 *     Determine whether cfg[] allows cursor caching.
 *     Optimized for common cases of NULL or "overwrite=false" config.
 */
static int
__cursors_can_be_cached(WT_SESSION_IMPL *session, const char *cfg[], bool *cacheablep)
{
    WT_CONFIG_ITEM cval;
    
    /*
     * Fast path #1: NULL or empty config means all defaults.
     * Default cursor is always cacheable.
     *
     * IMPORTANT: This logic must match __wt_cursor_cache_get (cur_std.c line 1042-1043):
     *   have_config = (cfg != NULL && cfg[0] != NULL && cfg[1] != NULL && 
     *                  (cfg[2] != NULL || cfg[1][0] != '\0'));
     *
     * We want fast path when !have_config, i.e., when:
     *   cfg == NULL OR cfg[0] == NULL OR cfg[1] == NULL OR 
     *   (cfg[2] == NULL AND cfg[1][0] == '\0')
     *
     * Check order matters for short-circuit evaluation to prevent NULL dereference.
     */
    if (cfg == NULL || cfg[0] == NULL || cfg[1] == NULL || 
        (cfg[2] == NULL && cfg[1][0] == '\0')) {
        *cacheablep = true;
        WT_STAT_CONN_INCR(session, cursor_config_fast_path_null);
        return (0);
    }
    
    /*
     * Fast path #2: "overwrite=false" is extremely common (MongoDB default).
     * This cursor is still cacheable.
     *
     * NOTE: This pattern already exists in __wt_cursor_cache_get() 
     * (src/cursor/cur_std.c lines 1050-1058). We extend it here to
     * __cursors_can_be_cached() for consistency and to catch more cases.
     *
     * IMPORTANT: cfg[1] != NULL is guaranteed by the fast path #1 check above
     * (if cfg[1] was NULL, we would have returned already).
     */
    if (cfg[1] != NULL && cfg[2] == NULL && strcmp(cfg[1], "overwrite=false") == 0) {
        *cacheablep = true;
        WT_STAT_CONN_INCR(session, cursor_config_fast_path_overwrite);
        return (0);
    }
    
    /*
     * Fast path #3: "overwrite=true" (explicit default).
     */
    if (cfg[1] != NULL && cfg[2] == NULL && strcmp(cfg[1], "overwrite=true") == 0) {
        *cacheablep = true;
        WT_STAT_CONN_INCR(session, cursor_config_fast_path_overwrite);
        return (0);
    }
    
    /* Slow path: parse all options */
    WT_STAT_CONN_INCR(session, cursor_config_slow_path);
    
    WT_RET(__wt_config_gets_def(session, cfg, "bulk", 0, &cval));
    if (cval.val)
        goto return_false;

    WT_RET(__wt_config_gets_def(session, cfg, "debug", 0, &cval));
    if (cval.len != 0)
        goto return_false;

    WT_RET(__wt_config_gets_def(session, cfg, "dump", 0, &cval));
    if (cval.len != 0)
        goto return_false;

    /* ... remaining checks ... */

    *cacheablep = true;
    return (0);
    
return_false:
    *cacheablep = false;
    return (0);
}
```

#### 5.2.2 Cursor Reuse Optimization

```c
/*
 * __cursor_reuse_or_init --
 *     Initialize cursor with config, optimized for common cases.
 */
static int
__cursor_reuse_or_init(WT_SESSION_IMPL *session, WT_CURSOR *cursor,
    const char *cfg[], bool *readonlyp, WT_CURSOR **ownerp, WT_CURSOR **cdumpp)
{
    WT_CONFIG_ITEM cval;
    
    /*
     * Fast path: NULL config means all defaults.
     * Set default flags directly without parsing.
     */
    if (cfg == NULL || cfg[0] == NULL || cfg[1] == NULL || cfg[1][0] == '\0') {
        F_CLR(cursor, WT_CURSTD_APPEND | WT_CURSTD_RAW);
        F_SET(cursor, WT_CURSTD_OVERWRITE);
        *readonlyp = false;
        if (cdumpp != NULL)
            *cdumpp = NULL;
        return (0);
    }
    
    /*
     * Fast path: "overwrite=false" only.
     */
    if (cfg[2] == NULL && strcmp(cfg[1], "overwrite=false") == 0) {
        F_CLR(cursor, WT_CURSTD_APPEND | WT_CURSTD_RAW | WT_CURSTD_OVERWRITE);
        *readonlyp = false;
        if (cdumpp != NULL)
            *cdumpp = NULL;
        return (0);
    }
    
    /* Slow path: parse all options */
    /* ... existing code ... */
}
```

---

## 6. Auto-Generation Strategy

### 6.1 Generator Scripts

```
dist/
├── gen_btree_conf.py       # Generate btree config cache structures
├── gen_cursor_conf.py      # Generate cursor config API
├── test_gen_btree_conf.py  # Unit tests for btree generator
└── test_gen_cursor_conf.py # Unit tests for cursor generator
```

### 6.2 Source Definitions

#### 6.2.1 Btree Config Options

Create `dist/btree_config_defs.py`:

```python
"""
Btree configuration option definitions.
Used by gen_btree_conf.py to generate cache structures.
"""

BTREE_CONFIG_OPTIONS = [
    # (config_name, c_type, cache_field, is_boolean, default_value, description)
    
    # String options (require strdup)
    ("key_format", "char *", "key_format", False, "u", "Key format string"),
    ("value_format", "char *", "value_format", False, "u", "Value format string"),
    ("collator", "char *", "collator", False, "", "Custom collator name"),
    ("block_compressor", "char *", "block_compressor", False, "", "Block compressor"),
    
    # Integer options
    ("id", "uint32_t", "id", False, 0, "File ID"),
    ("allocation_size", "uint32_t", "allocsize", False, 4096, "Allocation unit size"),
    ("internal_page_max", "uint32_t", "maxintlpage", False, 4096, "Max internal page"),
    ("leaf_page_max", "uint32_t", "maxleafpage", False, 32768, "Max leaf page size"),
    ("memory_page_max", "uint64_t", "maxmempage", False, 5242880, "Max in-memory page"),
    ("split_pct", "uint32_t", "split_pct", False, 90, "Split percentage"),
    
    # Boolean options (stored as flags)
    ("cache_resident", "bool", "CACHE_RESIDENT", True, False, "Keep in cache"),
    ("ignore_in_memory_cache_size", "bool", "IGNORE_CACHE_SIZE", True, False, "Ignore cache"),
    ("in_memory", "bool", "IN_MEMORY", True, False, "In-memory table"),
    ("prefix_compression", "bool", "PREFIX_COMPRESSION", True, False, "Prefix compression"),
    ("internal_key_truncate", "bool", "INTERNAL_KEY_TRUNCATE", True, True, "Truncate keys"),
    
    # ... remaining options
]
```

#### 6.2.2 Cursor Config Options

Extract from existing `dist/api_data.py`:

```python
# In api_data.py, cursor_config section contains:
cursor_config = [
    Config('append', 'false', r'''
        append inserted values as new records...'''),
    Config('bulk', 'false', r'''
        configure the cursor for bulk-loading...'''),
    Config('checkpoint', '', r'''
        the name of a checkpoint to open...'''),
    # ... etc
]
```

### 6.3 Generated Output

#### 6.3.1 Btree Config (wiredtiger_btree_conf.h)

```c
/* AUTO-GENERATED FILE - DO NOT EDIT */

#ifndef WT_BTREE_CONF_H
#define WT_BTREE_CONF_H

/*
 * WT_BTREE_CONF --
 *     Cached btree configuration structure.
 *     Generated from dist/btree_config_defs.py
 */
struct __wt_btree_conf {
    bool parsed;
    
    /* String options (owned copies) */
    char *key_format;
    char *value_format;
    char *collator_name;      /* Collator name string, not object pointer */
    char *block_compressor;
    /* ... */
    
    /* Integer options */
    uint32_t id;
    uint32_t allocsize;
    uint32_t maxintlpage;
    uint32_t maxleafpage;
    uint64_t maxmempage;
    uint32_t split_pct;
    /* ... */
    
    /* Boolean flags */
    uint32_t flags;
#define WT_BTREE_CONF_CACHE_RESIDENT        0x00001u
#define WT_BTREE_CONF_IGNORE_CACHE_SIZE     0x00002u
#define WT_BTREE_CONF_IN_MEMORY             0x00004u
#define WT_BTREE_CONF_PREFIX_COMPRESSION    0x00008u
#define WT_BTREE_CONF_INTERNAL_KEY_TRUNCATE 0x00010u
    /* ... */
};

#endif /* WT_BTREE_CONF_H */
```

---

## 7. Testing Strategy

### 7.1 Test Categories

| Category | Purpose | Coverage |
|----------|---------|----------|
| **Correctness** | Values match string parsing | 100% of options |
| **Cache behavior** | Hit/miss/invalidation | All paths |
| **API equivalence** | New API = old API behavior | All options |
| **Concurrency** | Thread safety | Multi-session |
| **Performance** | Measurable improvement | Benchmarks |
| **Edge cases** | Boundary conditions | All types |

### 7.2 Test Files

```
test/csuite/
├── wt_btree_conf/
│   ├── main.c              # Btree cache tests
│   └── CMakeLists.txt
├── wt_cursor_conf/
│   ├── main.c              # Cursor API tests
│   ├── comparison_test.c   # API equivalence tests
│   └── CMakeLists.txt
└── wt_conf_perf/
    ├── main.c              # Performance benchmarks
    └── CMakeLists.txt
```

### 7.3 Btree Cache Tests

```c
/* test/csuite/wt_btree_conf/main.c */

/*
 * test_cache_populated_once --
 *     Verify cache is populated exactly once per dhandle.
 */
static void
test_cache_populated_once(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session1, *session2;
    WT_CURSOR *cursor;
    
    /* Create connection and table */
    testutil_check(wiredtiger_open(home, NULL, "create", &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session1));
    testutil_check(session1->create(session1, uri, table_config));
    
    /* First open - should populate cache */
    testutil_check(session1->open_cursor(session1, uri, NULL, NULL, &cursor));
    verify_stat(session1, WT_STAT_CONN_BTREE_CONF_CACHE_MISS, 1);
    testutil_check(cursor->close(cursor));
    
    /* Second open same session - should hit cache */
    testutil_check(session1->open_cursor(session1, uri, NULL, NULL, &cursor));
    verify_stat(session1, WT_STAT_CONN_BTREE_CONF_CACHE_HIT, 1);
    testutil_check(cursor->close(cursor));
    
    /* Different session - should still hit cache (dhandle shared) */
    testutil_check(conn->open_session(conn, NULL, NULL, &session2));
    testutil_check(session2->open_cursor(session2, uri, NULL, NULL, &cursor));
    verify_stat(session2, WT_STAT_CONN_BTREE_CONF_CACHE_HIT, 2);
    testutil_check(cursor->close(cursor));
    
    /* Cache miss count should still be 1 */
    verify_stat(session2, WT_STAT_CONN_BTREE_CONF_CACHE_MISS, 1);
    
    testutil_check(conn->close(conn, NULL));
}

/*
 * test_cache_values_correct --
 *     Verify all cached values match string-parsed values.
 */
static void
test_cache_values_correct(void)
{
    /* Create table with non-default config */
    const char *config = 
        "key_format=S,value_format=u,"
        "allocation_size=8192,"
        "leaf_page_max=65536,"
        "prefix_compression=true,"
        "cache_resident=true";
    
    /* Open once to populate cache */
    testutil_check(session->create(session, uri, config));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    
    /* Get btree handle and verify values */
    WT_CURSOR_BTREE *cbt = (WT_CURSOR_BTREE *)cursor;
    WT_BTREE *btree = CUR2BT(cbt);
    
    testutil_assert(strcmp(btree->key_format, "S") == 0);
    testutil_assert(strcmp(btree->value_format, "u") == 0);
    testutil_assert(btree->allocsize == 8192);
    testutil_assert(btree->maxleafpage == 65536);
    testutil_assert(F_ISSET(btree, WT_BTREE_NO_EVICT));  /* cache_resident */
    
    testutil_check(cursor->close(cursor));
    
    /* Reopen from cache and verify same values */
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    cbt = (WT_CURSOR_BTREE *)cursor;
    btree = CUR2BT(cbt);
    
    testutil_assert(strcmp(btree->key_format, "S") == 0);
    testutil_assert(btree->allocsize == 8192);
    /* ... verify all values ... */
}

/*
 * test_cache_invalidation_on_alter --
 *     Verify cache is invalidated when table is altered.
 */
static void
test_cache_invalidation_on_alter(void)
{
    /* Create and open table */
    testutil_check(session->create(session, uri, "cache_resident=false"));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    
    WT_BTREE *btree = CUR2BT((WT_CURSOR_BTREE *)cursor);
    testutil_assert(!F_ISSET(btree, WT_BTREE_NO_EVICT));
    testutil_check(cursor->close(cursor));
    
    /* Alter table */
    testutil_check(session->alter(session, uri, "cache_resident=true"));
    
    /* Reopen - should see new config */
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    btree = CUR2BT((WT_CURSOR_BTREE *)cursor);
    testutil_assert(F_ISSET(btree, WT_BTREE_NO_EVICT));  /* Now true */
    
    testutil_check(cursor->close(cursor));
}

/*
 * test_concurrent_alter_read --
 *     Verify cache handles concurrent alter + read operations safely.
 */
static void
test_concurrent_alter_read(void)
{
    pthread_t readers[4], alter_thread;
    
    /* Create table */
    testutil_check(session->create(session, uri, "cache_resident=false"));
    
    /* Start reader threads */
    for (int i = 0; i < 4; i++)
        testutil_check(pthread_create(&readers[i], NULL, reader_thread_func, NULL));
    
    /* Start alter thread */
    testutil_check(pthread_create(&alter_thread, NULL, alter_thread_func, NULL));
    
    /* Wait for completion */
    pthread_join(alter_thread, NULL);
    for (int i = 0; i < 4; i++)
        pthread_join(readers[i], NULL);
    
    /* Verify no crashes, data corruption */
    verify_table_consistency(session, uri);
}

/*
 * test_cache_population_error_recovery --
 *     Verify cache handles allocation failures gracefully.
 *
 * NOTE: This test uses pseudocode for fault injection. The actual
 * implementation should use WiredTiger's existing failpoint infrastructure:
 *   - timing_stress_for_test configuration option
 *   - WT_SESSION::reconfigure with specific failpoint flags
 *   - Or the WT_DIAGNOSTIC-enabled test hooks
 * 
 * See test/csuite and test/suite for existing examples of error injection.
 */
static void
test_cache_population_error_recovery(void)
{
    /*
     * Use fault injection to simulate __wt_strndup failure mid-population.
     * Verify:
     * 1. No memory leaks (partially-copied strings freed)
     * 2. Cache remains unpopulated (not in corrupt state)
     * 3. Subsequent open succeeds
     */
    testutil_check(session->create(session, uri, table_config));
    
    /* 
     * PSEUDOCODE: Enable fault injection for string allocation.
     * Actual implementation would use WiredTiger's failpoint mechanism.
     */
    /* enable_fault_injection(WT_FAULT_STRNDUP); */
    
    /* First open should fail gracefully */
    int ret = session->open_cursor(session, uri, NULL, NULL, &cursor);
    testutil_assert(ret == ENOMEM);
    
    /* PSEUDOCODE: Disable fault injection */
    /* disable_fault_injection(WT_FAULT_STRNDUP); */
    
    /* Next open should succeed and populate cache */
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    verify_stat(session, WT_STAT_CONN_BTREE_CONF_CACHE_MISS, 1);
    testutil_check(cursor->close(cursor));
}

/*
 * test_long_running_session --
 *     Verify cache doesn't grow unbounded over time.
 */
static void
test_long_running_session(void)
{
    const int num_tables = 100;
    const int iterations = 1000;
    
    /* Create many tables */
    for (int t = 0; t < num_tables; t++) {
        char uri[64];
        snprintf(uri, sizeof(uri), "table:test%d", t);
        testutil_check(session->create(session, uri, table_config));
    }
    
    /* Repeatedly open/close cursors on random tables */
    for (int i = 0; i < iterations; i++) {
        char uri[64];
        snprintf(uri, sizeof(uri), "table:test%d", rand() % num_tables);
        
        testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
        testutil_check(cursor->close(cursor));
    }
    
    /* Verify cache count is bounded by num_tables */
    uint64_t cache_misses;
    get_stat(session, WT_STAT_CONN_BTREE_CONF_CACHE_MISS, &cache_misses);
    testutil_assert(cache_misses <= num_tables);
}
```

### 7.4 Cursor API Tests

```c
/* test/csuite/wt_cursor_conf/main.c */

/*
 * test_overwrite_false_struct --
 *     Verify overwrite=false via struct API matches string API.
 */
static void
test_overwrite_false_struct(void)
{
    WT_CURSOR *cursor_string, *cursor_struct;
    
    /* Open with string config */
    testutil_check(session->open_cursor(
        session, uri, NULL, "overwrite=false", &cursor_string));
    
    /* Open with struct config */
    WT_CURSOR_CONFIG_ARG config[] = {
        WT_CURSOR_CONFIG_ARG_SET_BOOL(WT_CURSOR_CONF_overwrite, false),
        WT_CURSOR_CONFIG_ARG_END
    };
    testutil_check(session->open_cursor_ex(
        session, uri, config, 0, &cursor_struct));
    
    /* Both should reject duplicate inserts */
    cursor_string->set_key(cursor_string, "key1");
    cursor_string->set_value(cursor_string, "value1");
    testutil_check(cursor_string->insert(cursor_string));
    
    cursor_struct->set_key(cursor_struct, "key2");
    cursor_struct->set_value(cursor_struct, "value2");
    testutil_check(cursor_struct->insert(cursor_struct));
    
    /* Duplicate insert should fail for both */
    cursor_string->set_key(cursor_string, "key1");
    cursor_string->set_value(cursor_string, "value1b");
    testutil_assert(cursor_string->insert(cursor_string) == WT_DUPLICATE_KEY);
    
    cursor_struct->set_key(cursor_struct, "key2");
    cursor_struct->set_value(cursor_struct, "value2b");
    testutil_assert(cursor_struct->insert(cursor_struct) == WT_DUPLICATE_KEY);
}

/*
 * test_all_options_equivalence --
 *     Verify every cursor option produces identical behavior between APIs.
 */
static void
test_all_options_equivalence(void)
{
    /* Test each option individually */
    test_option_equivalence("append=true", 
        WT_CURSOR_CONF_append, true, WT_CONF_TYPE_BOOL);
    test_option_equivalence("raw=true",
        WT_CURSOR_CONF_raw, true, WT_CONF_TYPE_BOOL);
    test_option_equivalence("read_once=true",
        WT_CURSOR_CONF_read_once, true, WT_CONF_TYPE_BOOL);
    /* ... test all options ... */
}
```

### 7.5 Performance Tests

```c
/* test/csuite/wt_conf_perf/main.c */

/*
 * bench_cursor_open --
 *     Benchmark cursor open with different config methods.
 */
static void
bench_cursor_open(void)
{
    const int iterations = 100000;
    const int warmup = 1000;
    WT_CURSOR *cursor;
    uint64_t start, end, elapsed_string, elapsed_struct, elapsed_null;
    
    printf("Cursor open benchmark (%d iterations)\n", iterations);
    printf("=========================================\n");
    
    /* Warmup */
    for (int i = 0; i < warmup; i++) {
        testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
        testutil_check(cursor->close(cursor));
    }
    
    /* Benchmark: NULL config (should hit fast path) */
    start = __wt_clock(NULL);
    for (int i = 0; i < iterations; i++) {
        testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
        testutil_check(cursor->close(cursor));
    }
    end = __wt_clock(NULL);
    elapsed_null = end - start;
    printf("NULL config:           %8.2f opens/sec\n",
        (double)iterations * WT_MILLION / elapsed_null);
    
    /* Benchmark: String config "overwrite=false" */
    start = __wt_clock(NULL);
    for (int i = 0; i < iterations; i++) {
        testutil_check(session->open_cursor(
            session, uri, NULL, "overwrite=false", &cursor));
        testutil_check(cursor->close(cursor));
    }
    end = __wt_clock(NULL);
    elapsed_string = end - start;
    printf("String 'overwrite=false': %8.2f opens/sec\n",
        (double)iterations * WT_MILLION / elapsed_string);
    
    /* Benchmark: Struct config */
    WT_CURSOR_CONFIG_ARG config[] = {
        WT_CURSOR_CONFIG_ARG_SET_BOOL(WT_CURSOR_CONF_overwrite, false),
        WT_CURSOR_CONFIG_ARG_END
    };
    start = __wt_clock(NULL);
    for (int i = 0; i < iterations; i++) {
        testutil_check(session->open_cursor_ex(session, uri, config, 0, &cursor));
        testutil_check(cursor->close(cursor));
    }
    end = __wt_clock(NULL);
    elapsed_struct = end - start;
    printf("Struct config:         %8.2f opens/sec\n",
        (double)iterations * WT_MILLION / elapsed_struct);
    
    /* Report speedup */
    printf("\nSpeedup vs string config:\n");
    printf("  NULL config:   %.2fx\n", (double)elapsed_string / elapsed_null);
    printf("  Struct config: %.2fx\n", (double)elapsed_string / elapsed_struct);
}

/*
 * bench_btree_open --
 *     Benchmark btree open with and without config cache.
 */
static void
bench_btree_open(void)
{
    const int iterations = 10000;
    WT_CURSOR *cursor;
    uint64_t start, end;
    
    printf("\nBtree open benchmark (%d iterations)\n", iterations);
    printf("=========================================\n");
    
    /* Create multiple tables to measure cache effectiveness */
    for (int t = 0; t < 10; t++) {
        char table_uri[64];
        snprintf(table_uri, sizeof(table_uri), "table:test%d", t);
        testutil_check(session->create(session, table_uri, table_config));
    }
    
    /* Benchmark: repeated opens of same tables (should hit cache) */
    start = __wt_clock(NULL);
    for (int i = 0; i < iterations; i++) {
        char table_uri[64];
        snprintf(table_uri, sizeof(table_uri), "table:test%d", i % 10);
        testutil_check(session->open_cursor(session, table_uri, NULL, NULL, &cursor));
        testutil_check(cursor->close(cursor));
    }
    end = __wt_clock(NULL);
    
    printf("Opens with cache:    %8.2f opens/sec\n",
        (double)iterations * WT_MILLION / (end - start));
    
    /* Report cache hit rate */
    uint64_t hits, misses;
    get_stat(session, WT_STAT_CONN_BTREE_CONF_CACHE_HIT, &hits);
    get_stat(session, WT_STAT_CONN_BTREE_CONF_CACHE_MISS, &misses);
    printf("Cache hit rate:      %.2f%%\n",
        100.0 * hits / (hits + misses));
}
```

---

## 8. Implementation Phases

### Phase 1: Fast Path for NULL Config (1-2 days)

**Goal**: Quick win with minimal risk.

**Changes**:
1. Add early return in `__cursors_can_be_cached()` for NULL/empty config
2. Add fast path for `"overwrite=false"` string
3. Add statistics counters

**Files modified**:
- `src/cursor/cur_std.c`
- `src/include/stat.h` (new stats)

**Tests**:
- Verify fast path triggered via statistics
- Verify behavior unchanged

### Phase 2A: Btree Config Cache - Simple Values (3-4 days)

**Goal**: Cache integer/boolean values, keep object lookups for collator/compressor.

**Changes**:
1. Create `WT_BTREE_CONF` structure (simple values only)
2. Add `btree_conf_cache` field to `WT_DATA_HANDLE`
3. Implement cache populate/use/free functions
4. Modify `__btree_conf()` to use cache for simple values
5. Keep collator/compressor lookup from cached name strings
6. Add cache invalidation in alter/drop/destroy paths
7. Add statistics counters
8. Add debug bypass flag

**Files modified**:
- `src/include/btree.h` (new struct)
- `src/include/dhandle.h` (new field)
- `src/btree/bt_handle.c` (main changes)
- `src/conn/conn_dhandle.c` (cleanup in __conn_dhandle_destroy)
- `src/schema/schema_alter.c` (invalidation)

**Tests**:
- Cache hit/miss verification
- Value correctness (all simple values)
- Invalidation on alter
- Concurrent access
- Error recovery (allocation failure)
- Long-running session (no unbounded growth)

### Phase 2B: Btree Config Cache - Object Pointers (Future, Optional)

**Goal**: Cache collator/compressor object pointers for maximum performance.

**Prerequisite**: Verify object lifetime guarantees.

**Changes**:
1. Add object pointers to `WT_BTREE_CONF`
2. Add ownership tracking
3. Modify cache population to store objects
4. Add reference counting if needed

**Deferred**: Only implement if Phase 2A profiling shows object lookups are still significant.

### Phase 3: Cursor Config API (3-4 days)

**Goal**: Zero-parsing cursor open for structured config.

**Changes**:
1. Create `gen_cursor_conf.py` generator
2. Generate `wiredtiger_cursor_conf.h`
3. Add `open_cursor_ex()` to `WT_SESSION`
4. Implement config parsing and validation
5. Integrate with cursor cache
6. Add statistics counters

**Files modified**:
- `dist/gen_cursor_conf.py` (new)
- `src/include/wiredtiger.h.in` (API)
- `src/include/wiredtiger_cursor_conf.h` (generated)
- `src/session/session_api.c` (implementation)
- `src/cursor/cur_std.c` (cache integration)

**Tests**:
- API equivalence for all options
- Type validation
- Cache integration
- Error handling

### Phase 4: Integration & Optimization (2-3 days)

**Goal**: Production readiness.

**Changes**:
1. Integration testing with MongoDB workloads
2. Performance benchmarking and profiling
3. Optimize based on profiling results
4. Documentation updates

**Deliverables**:
- Benchmark results showing improvement
- Updated documentation
- Integration guide for MongoDB

---

## 9. Statistics and Monitoring

### 9.1 New Statistics

```c
/* Connection statistics - add to stat.h */
/*
 * NOTE: Verify naming convention against existing statistics in
 * src/include/stat.h before implementation. Existing stats use
 * patterns like:
 *   WT_STAT_CONN_CURSOR_CACHE_HIT
 *   WT_STAT_CONN_CURSOR_CACHE_MISS
 *   WT_STAT_CONN_CURSOR_CREATE
 * 
 * The proposed names below follow this convention.
 */

/* Btree config cache */
WT_STAT_CONN_BTREE_CONF_CACHE_HIT      /* Cache hits */
WT_STAT_CONN_BTREE_CONF_CACHE_MISS     /* Cache misses (first open) */
WT_STAT_CONN_BTREE_CONF_CACHE_EVICT    /* Cache evictions */

/* Cursor config fast paths */
WT_STAT_CONN_CURSOR_CONFIG_FAST_PATH_NULL       /* NULL config fast path */
WT_STAT_CONN_CURSOR_CONFIG_FAST_PATH_OVERWRITE  /* "overwrite=" fast path */
WT_STAT_CONN_CURSOR_CONFIG_SLOW_PATH            /* Full parsing required */

/* Cursor API usage */
WT_STAT_CONN_CURSOR_OPEN_STRING_API    /* open_cursor() calls */
WT_STAT_CONN_CURSOR_OPEN_STRUCT_API    /* open_cursor_ex() calls */
```

### 9.2 Monitoring Queries

```python
# Example: Check cache effectiveness
def check_btree_cache_effectiveness(conn):
    cursor = session.open_cursor("statistics:")
    hits = get_stat(cursor, "btree_conf_cache_hit")
    misses = get_stat(cursor, "btree_conf_cache_miss")
    hit_rate = hits / (hits + misses) * 100
    print(f"Btree config cache hit rate: {hit_rate:.2f}%")
    # Target: >99% in steady state

def check_cursor_fast_path_usage(conn):
    cursor = session.open_cursor("statistics:")
    null_fast = get_stat(cursor, "cursor_config_fast_path_null")
    overwrite_fast = get_stat(cursor, "cursor_config_fast_path_overwrite")
    slow = get_stat(cursor, "cursor_config_slow_path")
    total = null_fast + overwrite_fast + slow
    fast_rate = (null_fast + overwrite_fast) / total * 100
    print(f"Cursor config fast path rate: {fast_rate:.2f}%")
    # Target: >80% for typical MongoDB workloads
```

---

## 10. Risks and Mitigations

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| Cache staleness after alter | Data corruption | Low | Explicit invalidation on all alter/drop/destroy paths; comprehensive tests |
| Memory overhead from cache | Increased footprint | Low | Cache is 300-500 bytes per table + ~100 bytes per btree open; freed on dhandle close |
| Thread safety issues | Crashes, corruption | Low | Cache protected by existing dhandle locks; defensive double-check pattern added |
| String double-free | Crashes | **Eliminated** | Always copy strings to btree; no shared ownership |
| NULL pointer dereference | Crashes | Low | Fixed in fast path code; explicit NULL checks |
| Collator/compressor complexity | Incorrect behavior | Low | Phase 2A caches names only, object lookup still performed |
| ABI compatibility break | MongoDB build failures | Low | New API is additive; old API unchanged |
| String lifetime in cursor config | Use-after-free | Medium | Document clearly; validate in debug builds |
| Performance regression | Slower than before | Low | Comprehensive benchmarks before/after; feature flag for rollback |

### 10.1 Rollback Plan

If issues are discovered post-deployment:

1. **Phase 1 (Fast paths)**: Revert changes to `cur_std.c`
2. **Phase 2 (Btree cache)**: Set `btree_conf_cache = NULL` to force slow path
3. **Phase 3 (Cursor API)**: Applications continue using `open_cursor()`

---

## 11. Success Metrics

| Metric | Baseline | Target | Measurement |
|--------|----------|--------|-------------|
| `__config_next` CPU % | 14.92% | <5% | MongoDB profiling |
| Btree conf cache hit rate | N/A | >99% | WiredTiger statistics |
| Cursor config fast path rate | N/A | >80% | WiredTiger statistics |
| Cursor open latency (NULL) | TBD | -50% | Microbenchmark |
| Cursor open latency (struct) | TBD | -80% | Microbenchmark |
| Memory overhead per table | 0 | 300-500 bytes | Memory profiling |
| Memory overhead per btree open | 0 | ~100 bytes | String copies |

---

## Appendix A: Configuration Options Reference

### A.1 Btree Configuration Options (~41 total)

| Option | Type | Cached Field | Notes |
|--------|------|--------------|-------|
| key_format | string | key_format | strdup required |
| value_format | string | value_format | strdup required |
| allocation_size | int | allocsize | |
| internal_page_max | int | maxintlpage | |
| leaf_page_max | int | maxleafpage | |
| memory_page_max | int | maxmempage | |
| cache_resident | bool | flags | |
| prefix_compression | bool | flags | |
| ... | ... | ... | See gen_btree_conf.py |

### A.2 Cursor Configuration Options (~15 total)

| Option | Type | Key ID | Default |
|--------|------|--------|---------|
| append | bool | 1001 | false |
| bulk | bool | 1002 | false |
| checkpoint | string | 1003 | "" |
| debug | string | 1004 | "" |
| dump | string | 1005 | "" |
| next_random | bool | 1006 | false |
| overwrite | bool | 1007 | **true** |
| raw | bool | 1008 | false |
| read_once | bool | 1009 | false |
| readonly | bool | 1010 | false |

---

## Appendix B: API Reference

### B.1 New Public APIs

```c
/* Cursor configuration keys */
#define WT_CURSOR_CONF_overwrite  1007

/* Configuration argument structure */
typedef struct __wt_cursor_config_arg {
    uint64_t key;
    uint8_t type;
    union { bool v_bool; int64_t v_int; struct { const char *str; size_t len; } v_str; };
} WT_CURSOR_CONFIG_ARG;

/* Helper macros */
#define WT_CURSOR_CONFIG_ARG_SET_BOOL(k, v) { .key = (k), .type = WT_CONF_TYPE_BOOL, .v_bool = (v) }
#define WT_CURSOR_CONFIG_ARG_END { .key = 0 }

/* New session method */
int WT_SESSION::open_cursor_ex(WT_SESSION *session, const char *uri,
    const WT_CURSOR_CONFIG_ARG *config, size_t config_count, WT_CURSOR **cursorp);
```

### B.2 Usage Example

```c
/* Before (string parsing on every call) */
session->open_cursor(session, "table:test", NULL, "overwrite=false", &cursor);

/* After (zero string parsing) */
WT_CURSOR_CONFIG_ARG config[] = {
    WT_CURSOR_CONFIG_ARG_SET_BOOL(WT_CURSOR_CONF_overwrite, false),
    WT_CURSOR_CONFIG_ARG_END
};
session->open_cursor_ex(session, "table:test", config, 0, &cursor);
```

---

## Appendix C: Review Feedback Response

This section documents how review feedback was incorporated into the design.

### C.1 Review 1 - Critical Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **Thread Safety Race** | Two sessions could race to populate cache | Added defensive double-check pattern after exclusive lock assertion (Section 3.7) |
| **String Ownership** | `__btree_clear()` frees strings, creating double-free risk | Changed design to always copy strings to btree (Section 3.4.2) |
| **Missing Collator/Compressor** | These are object pointers, not just strings | Added phased approach: Phase 2A caches strings, lookups still performed (Section 3.5) |
| **Cache Cleanup Missing** | No explicit cleanup in `__conn_dhandle_destroy()` | Added explicit cleanup code location (Section 3.6) |

### C.2 Review 1 - Moderate Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **NULL dereference** | Fast path assumed `cfg[1] != NULL` | Added explicit NULL check (Section 5.2.1) |
| **Type inconsistency** | Different naming than `WT_OPEN_CONFIG_ARG` | Aligned naming convention, added documentation (Section 4.2.1) |
| **Memory estimate** | "~200 bytes" was too low | Updated to 300-500 bytes per table + ~100 bytes per btree open |
| **Missing tests** | Concurrent alter, error recovery, long-running | Added test cases (Section 7.3) |

### C.3 Review 1 - Suggestions Adopted

| Suggestion | Status | Notes |
|------------|--------|-------|
| Always copy strings | **Adopted** | Eliminates ownership complexity |
| Cache version field | Deferred | Not required given explicit invalidation |
| Phased implementation | **Adopted** | Phase 2A (simple values) → Phase 2B (object pointers) |
| Debug bypass flag | **Adopted** | Added `debug_mode=(btree_conf_cache=false)` |

### C.4 Review 2 - Critical Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **Naming inconsistency** | `collator` vs `collator_name` used inconsistently | Standardized to `collator_name` throughout |
| **Cache invalidation location** | Should use `__conn_dhandle_config_clear()` | Updated to hook into existing config clearing (Section 3.6) |
| **Type system alignment** | `WT_CONF_TYPE_*` vs `WT_OPEN_CONFIG_ARG_*` | Aligned with existing pattern: `WT_CURSOR_CONFIG_ARG_*` (Section 4.2.1) |
| **flush_most_recent_ts type** | Document showed `wt_timestamp_t`, actual is `uint64_t` | Fixed to match btree.h |

### C.5 Review 2 - Medium Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **Undefined helpers** | `__cursor_config_hash()` etc. not defined | Added full implementations (Section 4.3.2) |
| **Fault injection** | `enable_fault_injection()` not real API | Noted as pseudocode, referenced actual failpoint mechanism |
| **NULL check comment** | Short-circuit evaluation not explained | Added detailed comment |

### C.6 Review 2 - Minor Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **Designated initializers** | Verify compiler support | Verified: pattern used in wiredtiger.h.in line 3627 |
| **Existing fast path** | Pattern exists in cur_std.c | Added reference to existing code (Section 5.2.1) |
| **Statistics names** | Verify naming convention | Added note to verify against src/include/stat.h |

### C.7 Review 3 - Critical Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **`__btree_conf` signature** | Document showed `(session, cfg[])`, actual is `(session, ckpt, is_ckpt)` | Fixed all occurrences to use correct signature |
| **Type mismatches** | `split_pct` should be `int`, `dictionary` etc. should be `u_int` | Fixed WT_BTREE_CONF to match btree.h exactly |
| **Missing fields** | `maxleafkey`, `maxleafvalue`, `internal_key_truncate`, `storage_tier`, `compressor_name` | Added missing fields to structure |

### C.8 Review 3 - Medium Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **Fast path NULL check** | Logic differs from `have_config` in cur_std.c | Aligned with actual `have_config` logic |
| **Optional config handling** | Missing `WT_RET_NOTFOUND_OK` documentation | Added detailed comments about required vs optional keys |
| **Hash function** | `__wt_hash_city32` doesn't exist | Changed to `__wt_hash_city64`, packed flags for efficiency |
| **Bucket lookup** | Hash recalculated vs parameter | Fixed to use `hash_value` parameter matching actual code |
| **Statistics names** | Need verification | Already noted, no change needed |

### C.9 Review 3 - Minor Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **Designated initializer syntax** | `.v_bool` vs `.value.v_int` | Fixed to match existing pattern with `.value.v_int` |
| **Fault injection** | Referenced non-existent API | Already noted as pseudocode |
| **Session method naming** | `__wt_` prefix for public API | Clarified as vtable method, not internal function |

### C.10 Review 4 - Critical Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **`collator_owned` type** | Should be `int`, not `bool` | Fixed to `int` with btree.h line reference |
| **Missing compressor lookup** | `__btree_conf_from_cache` didn't show compressor handling | Added explicit `__wt_compressor_config` call |
| **Incomplete error cleanup** | Only freed 3 strings, needed all | Fixed - only key_format/value_format are copied to btree |
| **`prefix_compression` flag** | Needed verification of flag setting | Already correct in code |

### C.11 Review 4 - Medium Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **Naming: `block_compressor`** | Inconsistent with `compressor_name` | Fixed to use `compressor_name` consistently |
| **Missing `kencryptor`** | No encryption handling shown | Added `__wt_btree_config_encryptor_from_cache` call |
| **`have_config` logic** | Potential NULL dereference | **[verified correct]** - short-circuit evaluation prevents it |
| **Missing `readonly`** | Flag not applied | Added `WT_BTREE_READONLY` flag application |
| **Derived values** | `maxleafkey`/`maxleafvalue` are derived, not simple | Don't cache - let `__btree_page_sizes()` compute them |
| **Missing `bitcnt`** | Fixed-length column store field | Added to WT_BTREE_CONF structure |

### C.12 Review 4 - Minor Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **Cursor config parsing** | `args[i].v_bool` vs `args[i].value.v_int` | Fixed to use `args[i].value.v_int != 0` |

### C.13 Review 5 - Critical Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **`__wt_collator_config` signature** | Missing `metadata` parameter | Fixed - reads `app_metadata` from dhandle->cfg |
| **Non-existent encryptor function** | `__wt_btree_config_encryptor_from_cache` doesn't exist | Fixed - use existing `__wt_btree_config_encryptor` with dhandle->cfg |
| **Union member access** | `args[i].v_str.str` should be `args[i].value.v_str.str` | Fixed all occurrences |

### C.14 Review 5 - Medium Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **Missing `app_metadata`** | Collator needs app_metadata | Fixed - read from dhandle->cfg during collator lookup |
| **Derived values** | More values than documented must NOT be cached | Added explicit list: `splitmempage`, `*_precomp`, `*_compadjust` |
| **Error path cleanup** | Relationship with __btree_clear unclear | Added clarifying comment |

### C.15 Review 5 - Minor Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **Type definition location** | WT_BTREE_CHECKSUM origin unclear | Added reference to btree.h lines 84-89 |
| **`prefix_compression_min`** | Missing from copy | **[verified]** - already present |
| **Debug flag naming** | Verify consistency | **[verified]** - follows existing pattern |

### C.16 Review 6 - Critical Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **`storage_tier` type** | Should be `WT_BTREE_STORAGE_TIER`, not `uint8_t` | Fixed to use actual enum type |
| **`maxmempage` runtime-adjusted** | Value depends on cache_size/dirty_trigger at runtime | Removed from cache, documented as runtime-adjusted |

### C.17 Review 6 - Medium Issues Addressed

| Issue | Review Concern | Resolution |
|-------|---------------|------------|
| **Missing `type` field** | btree->type derived from key_format | Added derivation in __btree_conf after cache load |
| **`checkpoint_config` goto** | Doesn't match actual code structure | Removed goto, restructured to linear flow |
| **Missing `__btree_page_sizes()`** | Must run after cache load | Added explicit call with detailed comment |
| **Cursor config hash fields** | Missing `append`, `read_once` | Documented: handled by post-cache patching per existing behavior |

### C.18 Design Trade-offs

| Trade-off | Choice | Rationale |
|-----------|--------|-----------|
| String sharing vs copying | **Copy** | +100 bytes per open, but eliminates all ownership bugs |
| Cache objects vs names | **Names only** (Phase 2A) | Safer, still eliminates ~35 of ~41 parses |
| Single-phase vs phased | **Phased** | Reduces risk, allows validation at each step |
| Type enum sharing | **Separate but aligned** | Backwards compatible, same values |
| Hash size | **64-bit** | Matches existing `__wt_hash_city64`, better distribution |
| Derived values | **Don't cache** | Let `__btree_page_sizes()` compute them as normal |
| Encryptor/collator config | **Read from dhandle->cfg** | These configs rarely change, cache name for fast-path skip |
| `maxmempage` | **Don't cache** | Runtime-adjusted based on cache_size/dirty_trigger |
| `btree->type` | **Derive after cache load** | Simple derivation from cached key_format |

---

*Document Version: 1.6*
*Last Updated: February 2026*
*Authors: Claude (Anthropic)*
*Revision: Incorporated review feedback (6 rounds)*
