/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * DO NOT EDIT: automatically built by dist/gen_open_conf.py.
 */

#include "wt_internal.h"
#include "wiredtiger_open_conf.h"

/*
 * Maps public WT_OPEN_CONF_* keys to internal configuration paths.
 *
 * For nested keys (e.g., eviction.threads_min), parent_name specifies
 * the parent category. The lookup code will:
 * 1. Get the parent config item
 * 2. Use __wt_config_subgets to get the nested value
 *
 * Note: Default values are NOT stored here. They are managed by the
 * existing configuration system (dist/api_config.py generates them
 * into the base config strings). For struct config, "key not present"
 * means "use default from base config".
 */
typedef struct {
    uint64_t public_key;      /* WT_OPEN_CONF_* */
    const char *key_name;     /* String name (e.g., "cache_size", "threads_min") */
    const char *parent_name;  /* Parent category (NULL if top-level) */
    uint8_t expected_type;    /* Expected WT_OPEN_CONFIG_ARG type */
} WT_OPEN_CONF_KEY_MAP;

/*
 * Key mappings for wiredtiger_open_ex.
 * All supported configuration keys.
 */
static const WT_OPEN_CONF_KEY_MAP open_conf_key_map[] = {
    /* ==========================================================================
     * Core options (1000-1099)
     * ========================================================================== */
    {WT_OPEN_CONF_cache_size, "cache_size", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_cache_overhead, "cache_overhead", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_cache_max_wait_ms, "cache_max_wait_ms", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_cache_stuck_timeout_ms, "cache_stuck_timeout_ms", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_checkpoint_sync, "checkpoint_sync", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_create, "create", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_error_prefix, "error_prefix", NULL, WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_in_memory, "in_memory", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_mmap, "mmap", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_mmap_all, "mmap_all", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_readonly, "readonly", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_salvage, "salvage", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_session_max, "session_max", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_session_scratch_max, "session_scratch_max", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_config_base, "config_base", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_cache_cursors, "cache_cursors", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_json_output, "json_output", NULL, WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_verbose, "verbose", NULL, WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_operation_timeout_ms, "operation_timeout_ms", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_generation_drain_timeout_ms, "generation_drain_timeout_ms", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_timing_stress_for_test, "timing_stress_for_test", NULL, WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_extra_diagnostics, "extra_diagnostics", NULL, WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_backup_restore_target, "backup_restore_target", NULL, WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_statistics, "statistics", NULL, WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_eviction_target, "eviction_target", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_eviction_trigger, "eviction_trigger", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_eviction_dirty_target, "eviction_dirty_target", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_eviction_dirty_trigger, "eviction_dirty_trigger", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_eviction_updates_target, "eviction_updates_target", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_eviction_updates_trigger, "eviction_updates_trigger", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_eviction_checkpoint_target, "eviction_checkpoint_target", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_buffer_alignment, "buffer_alignment", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_builtin_extension_config, "builtin_extension_config", NULL, WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_compile_configuration_count, "compile_configuration_count", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_direct_io, "direct_io", NULL, WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_disaggregated_checkpoint_meta, "checkpoint_meta", "disaggregated", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_disaggregated_drain_threads, "drain_threads", "disaggregated", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_disaggregated_last_materialized_lsn, "last_materialized_lsn", "disaggregated", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_disaggregated_local_files_action, "local_files_action", "disaggregated", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_disaggregated_lose_all_my_data, "lose_all_my_data", "disaggregated", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_disaggregated_role, "role", "disaggregated", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_disaggregated_page_log, "page_log", "disaggregated", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_exclusive, "exclusive", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_extensions, "extensions", NULL, WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_file_extend, "file_extend", NULL, WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_hazard_max, "hazard_max", NULL, WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_heuristic_controls_checkpoint_cleanup_obsolete_tw_pages_dirty_max, "checkpoint_cleanup_obsolete_tw_pages_dirty_max", "heuristic_controls", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_heuristic_controls_eviction_obsolete_tw_pages_dirty_max, "eviction_obsolete_tw_pages_dirty_max", "heuristic_controls", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_heuristic_controls_obsolete_tw_btree_max, "obsolete_tw_btree_max", "heuristic_controls", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_multiprocess, "multiprocess", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_operation_tracking_enabled, "enabled", "operation_tracking", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_operation_tracking_path, "path", "operation_tracking", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_page_delta_delta_pct, "delta_pct", "page_delta", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_page_delta_internal_page_delta, "internal_page_delta", "page_delta", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_page_delta_leaf_page_delta, "leaf_page_delta", "page_delta", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_page_delta_max_consecutive_delta, "max_consecutive_delta", "page_delta", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_precise_checkpoint, "precise_checkpoint", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_preserve_prepared, "preserve_prepared", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_rollback_to_stable_threads, "threads", "rollback_to_stable", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_session_table_cache, "session_table_cache", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_use_environment, "use_environment", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_use_environment_priv, "use_environment_priv", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_verify_metadata, "verify_metadata", NULL, WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_write_through, "write_through", NULL, WT_OPEN_CONFIG_ARG_STR},
    /* ==========================================================================
     * Eviction options (1100-1199)
     * ========================================================================== */
    {WT_OPEN_CONF_eviction_threads_min, "threads_min", "eviction", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_eviction_threads_max, "threads_max", "eviction", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_eviction_evict_sample_inmem, "evict_sample_inmem", "eviction", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_eviction_evict_use_softptr, "evict_use_softptr", "eviction", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_eviction_legacy_page_visit_strategy, "legacy_page_visit_strategy", "eviction", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_eviction_app_eviction_min_cache_fill_ratio, "app_eviction_min_cache_fill_ratio", "eviction", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_eviction_cache_tolerance_for_app_eviction, "cache_tolerance_for_app_eviction", "eviction", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_eviction_incremental_app_eviction, "incremental_app_eviction", "eviction", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_eviction_prefer_scrub_eviction, "prefer_scrub_eviction", "eviction", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_eviction_skip_update_obsolete_check, "skip_update_obsolete_check", "eviction", WT_OPEN_CONFIG_ARG_BOOL},
    /* ==========================================================================
     * Log options (1200-1299)
     * ========================================================================== */
    {WT_OPEN_CONF_log_enabled, "enabled", "log", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_log_archive, "archive", "log", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_log_path, "path", "log", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_log_file_max, "file_max", "log", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_log_prealloc, "prealloc", "log", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_log_zero_fill, "zero_fill", "log", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_log_compressor, "compressor", "log", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_log_remove, "remove", "log", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_log_recover, "recover", "log", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_log_os_cache_dirty_pct, "os_cache_dirty_pct", "log", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_log_prealloc_init_count, "prealloc_init_count", "log", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_log_force_write_wait, "force_write_wait", "log", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_log_recovery_skip, "recovery_skip", "log", WT_OPEN_CONFIG_ARG_BOOL},
    /* ==========================================================================
     * Checkpoint options (1300-1399)
     * ========================================================================== */
    {WT_OPEN_CONF_checkpoint_wait, "wait", "checkpoint", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_checkpoint_log_size, "log_size", "checkpoint", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_checkpoint_cleanup_wait, "wait", "checkpoint_cleanup", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_checkpoint_cleanup_method, "method", "checkpoint_cleanup", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_checkpoint_cleanup_file_wait_ms, "file_wait_ms", "checkpoint_cleanup", WT_OPEN_CONFIG_ARG_INT},
    /* ==========================================================================
     * Statistics options (1400-1499)
     * ========================================================================== */
    {WT_OPEN_CONF_statistics_log_wait, "wait", "statistics_log", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_statistics_log_on_close, "on_close", "statistics_log", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_statistics_log_path, "path", "statistics_log", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_statistics_log_sources, "sources", "statistics_log", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_statistics_log_timestamp, "timestamp", "statistics_log", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_statistics_log_json, "json", "statistics_log", WT_OPEN_CONFIG_ARG_BOOL},
    /* ==========================================================================
     * File Manager options (1500-1599)
     * ========================================================================== */
    {WT_OPEN_CONF_file_manager_close_idle_time, "close_idle_time", "file_manager", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_file_manager_close_scan_interval, "close_scan_interval", "file_manager", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_file_manager_close_handle_minimum, "close_handle_minimum", "file_manager", WT_OPEN_CONFIG_ARG_INT},
    /* ==========================================================================
     * Debug Mode options (1600-1699)
     * ========================================================================== */
    {WT_OPEN_CONF_debug_mode_corruption_abort, "corruption_abort", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_eviction, "eviction", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_cursor_copy, "cursor_copy", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_table_logging, "table_logging", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_checkpoint_retention, "checkpoint_retention", "debug_mode", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_debug_mode_configuration, "configuration", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_background_compact, "background_compact", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_crash_point_colgroup, "crash_point_colgroup", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_cursor_reposition, "cursor_reposition", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_disagg_address_cookie_upgrade, "disagg_address_cookie_upgrade", "debug_mode", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_debug_mode_disagg_address_cookie_optional_field, "disagg_address_cookie_optional_field", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_log_retention, "log_retention", "debug_mode", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_debug_mode_page_history, "page_history", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_realloc_exact, "realloc_exact", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_realloc_malloc, "realloc_malloc", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_rollback_error, "rollback_error", "debug_mode", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_debug_mode_slow_checkpoint, "slow_checkpoint", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_stress_skiplist, "stress_skiplist", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_tiered_flush_error_continue, "tiered_flush_error_continue", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_update_restore_evict, "update_restore_evict", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_debug_mode_eviction_checkpoint_ts_ordering, "eviction_checkpoint_ts_ordering", "debug_mode", WT_OPEN_CONFIG_ARG_BOOL},
    /* ==========================================================================
     * Compatibility options (1700-1799)
     * ========================================================================== */
    {WT_OPEN_CONF_compatibility_require_min, "require_min", "compatibility", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_compatibility_require_max, "require_max", "compatibility", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_compatibility_release, "release", "compatibility", WT_OPEN_CONFIG_ARG_STR},
    /* ==========================================================================
     * Prefetch options (1800-1899)
     * ========================================================================== */
    {WT_OPEN_CONF_prefetch_available, "available", "prefetch", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_prefetch_default, "default", "prefetch", WT_OPEN_CONFIG_ARG_BOOL},
    /* ==========================================================================
     * Live Restore options (1900-1999)
     * ========================================================================== */
    {WT_OPEN_CONF_live_restore_enabled, "enabled", "live_restore", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_live_restore_path, "path", "live_restore", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_live_restore_threads_max, "threads_max", "live_restore", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_live_restore_read_size, "read_size", "live_restore", WT_OPEN_CONFIG_ARG_INT},
    /* ==========================================================================
     * Encryption options (2000-2099)
     * ========================================================================== */
    {WT_OPEN_CONF_encryption_name, "name", "encryption", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_encryption_keyid, "keyid", "encryption", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_encryption_secretkey, "secretkey", "encryption", WT_OPEN_CONFIG_ARG_STR},
    /* ==========================================================================
     * Chunk Cache options (2100-2199)
     * ========================================================================== */
    {WT_OPEN_CONF_chunk_cache_pinned, "pinned", "chunk_cache", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_chunk_cache_capacity, "capacity", "chunk_cache", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_chunk_cache_chunk_cache_evict_trigger, "chunk_cache_evict_trigger", "chunk_cache", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_chunk_cache_chunk_size, "chunk_size", "chunk_cache", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_chunk_cache_storage_path, "storage_path", "chunk_cache", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_chunk_cache_enabled, "enabled", "chunk_cache", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_chunk_cache_hashsize, "hashsize", "chunk_cache", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_chunk_cache_flushed_data_cache_insertion, "flushed_data_cache_insertion", "chunk_cache", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_chunk_cache_type, "type", "chunk_cache", WT_OPEN_CONFIG_ARG_STR},
    /* ==========================================================================
     * Tiered options (2200-2299)
     * ========================================================================== */
    {WT_OPEN_CONF_tiered_storage_local_retention, "local_retention", "tiered_storage", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_tiered_storage_auth_token, "auth_token", "tiered_storage", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_tiered_storage_bucket, "bucket", "tiered_storage", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_tiered_storage_bucket_prefix, "bucket_prefix", "tiered_storage", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_tiered_storage_cache_directory, "cache_directory", "tiered_storage", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_tiered_storage_interval, "interval", "tiered_storage", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_tiered_storage_name, "name", "tiered_storage", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_tiered_storage_shared, "shared", "tiered_storage", WT_OPEN_CONFIG_ARG_BOOL},
    /* ==========================================================================
     * Hash options (2300-2399)
     * ========================================================================== */
    {WT_OPEN_CONF_hash_buckets, "buckets", "hash", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_hash_dhandle_buckets, "dhandle_buckets", "hash", WT_OPEN_CONFIG_ARG_INT},
    /* ==========================================================================
     * Transaction Sync options (2400-2499)
     * ========================================================================== */
    {WT_OPEN_CONF_transaction_sync_enabled, "enabled", "transaction_sync", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_transaction_sync_method, "method", "transaction_sync", WT_OPEN_CONFIG_ARG_STR},
    /* ==========================================================================
     * Io Capacity options (2500-2599)
     * ========================================================================== */
    {WT_OPEN_CONF_io_capacity_total, "total", "io_capacity", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_io_capacity_chunk_cache, "chunk_cache", "io_capacity", WT_OPEN_CONFIG_ARG_INT},
    /* ==========================================================================
     * History Store options (2600-2699)
     * ========================================================================== */
    {WT_OPEN_CONF_history_store_file_max, "file_max", "history_store", WT_OPEN_CONFIG_ARG_INT},
    /* ==========================================================================
     * Shared Cache options (2700-2799)
     * ========================================================================== */
    {WT_OPEN_CONF_shared_cache_chunk, "chunk", "shared_cache", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_shared_cache_name, "name", "shared_cache", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_shared_cache_quota, "quota", "shared_cache", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_shared_cache_reserve, "reserve", "shared_cache", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_shared_cache_size, "size", "shared_cache", WT_OPEN_CONFIG_ARG_INT},
    /* ==========================================================================
     * Block Cache options (2800-2899)
     * ========================================================================== */
    {WT_OPEN_CONF_block_cache_cache_on_checkpoint, "cache_on_checkpoint", "block_cache", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_block_cache_cache_on_writes, "cache_on_writes", "block_cache", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_block_cache_enabled, "enabled", "block_cache", WT_OPEN_CONFIG_ARG_BOOL},
    {WT_OPEN_CONF_block_cache_blkcache_eviction_aggression, "blkcache_eviction_aggression", "block_cache", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_block_cache_full_target, "full_target", "block_cache", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_block_cache_size, "size", "block_cache", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_block_cache_hashsize, "hashsize", "block_cache", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_block_cache_max_percent_overhead, "max_percent_overhead", "block_cache", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_block_cache_nvram_path, "nvram_path", "block_cache", WT_OPEN_CONFIG_ARG_STR},
    {WT_OPEN_CONF_block_cache_percent_file_in_dram, "percent_file_in_dram", "block_cache", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_block_cache_system_ram, "system_ram", "block_cache", WT_OPEN_CONFIG_ARG_INT},
    {WT_OPEN_CONF_block_cache_type, "type", "block_cache", WT_OPEN_CONFIG_ARG_STR},

    /* Terminator */
    {WT_OPEN_CONF_KEY_END, NULL, NULL, 0}
};

/*
 * __wt_open_conf_key_lookup --
 *     Look up a public key ID in the mapping table.
 *     Returns NULL if not found.
 *
 * Performance: Linear scan. For ~100 keys this is negligible.
 * The table is sorted by public_key so binary search could be
 * added if needed, but startup config lookup is not a hot path.
 */
static const WT_OPEN_CONF_KEY_MAP *
__wt_open_conf_key_lookup(uint64_t public_key)
{
    const WT_OPEN_CONF_KEY_MAP *entry;

    for (entry = open_conf_key_map; entry->key_name != NULL; entry++) {
        if (entry->public_key == public_key)
            return (entry);
    }
    return (NULL);
}

/*
 * __wt_open_conf_type_name --
 *     Return a human-readable name for a config arg type.
 */
static const char *
__wt_open_conf_type_name(uint8_t type)
{
    switch (type) {
    case WT_OPEN_CONFIG_ARG_INT:
        return ("integer");
    case WT_OPEN_CONFIG_ARG_BOOL:
        return ("boolean");
    case WT_OPEN_CONFIG_ARG_STR:
        return ("string");
    default:
        return ("unknown");
    }
}

/*
 * __wt_open_conf_validate_args --
 *     Validate all config args have known keys, correct types, and no duplicates.
 */
int
__wt_open_conf_validate_args(
  WT_SESSION_IMPL *session, const WT_OPEN_CONFIG_ARG *args, size_t count)
{
    const WT_OPEN_CONFIG_ARG *arg;
    const WT_OPEN_CONF_KEY_MAP *key_map;
    size_t i, j, n_args;

    if (args == NULL)
        return (0);

    /* First pass: count args and validate keys/types */
    if (count == 0) {
        /* Sentinel-terminated array - count and validate */
        n_args = 0;
        for (arg = args; arg->key != WT_OPEN_CONF_KEY_END; arg++) {
            key_map = __wt_open_conf_key_lookup(arg->key);
            if (key_map == NULL)
                WT_RET_MSG(session, EINVAL, "Unknown configuration key ID: %" PRIu64, arg->key);

            /* Type validation - allow bool where int expected */
            if (arg->type != key_map->expected_type) {
                if (!(key_map->expected_type == WT_OPEN_CONFIG_ARG_INT &&
                      arg->type == WT_OPEN_CONFIG_ARG_BOOL)) {
                    WT_RET_MSG(session, EINVAL,
                      "Configuration key %s (ID %" PRIu64 "): expected type %s, got %s",
                      key_map->key_name, arg->key, __wt_open_conf_type_name(key_map->expected_type),
                      __wt_open_conf_type_name(arg->type));
                }
            }
            n_args++;
        }
    } else {
        /* Counted array - validate */
        n_args = count;
        for (i = 0; i < count; i++) {
            arg = &args[i];
            key_map = __wt_open_conf_key_lookup(arg->key);
            if (key_map == NULL)
                WT_RET_MSG(session, EINVAL, "Unknown configuration key ID: %" PRIu64, arg->key);

            if (arg->type != key_map->expected_type) {
                if (!(key_map->expected_type == WT_OPEN_CONFIG_ARG_INT &&
                      arg->type == WT_OPEN_CONFIG_ARG_BOOL)) {
                    WT_RET_MSG(session, EINVAL,
                      "Configuration key %s (ID %" PRIu64 "): expected type %s, got %s",
                      key_map->key_name, arg->key, __wt_open_conf_type_name(key_map->expected_type),
                      __wt_open_conf_type_name(arg->type));
                }
            }
        }
    }

    /* Second pass: check for duplicate keys (O(n²) but n is small and startup-only) */
    for (i = 0; i < n_args; i++) {
        for (j = i + 1; j < n_args; j++) {
            if (args[i].key == args[j].key) {
                key_map = __wt_open_conf_key_lookup(args[i].key);
                WT_RET_MSG(session, EINVAL,
                  "Duplicate configuration key %s (ID %" PRIu64 ") at positions %zu and %zu",
                  key_map != NULL ? key_map->key_name : "unknown", args[i].key, i, j);
            }
        }
    }

    return (0);
}

/*
 * __wt_open_conf_get_key_info --
 *     Get the key name and parent name for a given key ID.
 *     Returns WT_NOTFOUND if key is unknown.
 */
int
__wt_open_conf_get_key_info(
  uint64_t key_id, const char **key_namep, const char **parent_namep, uint8_t *expected_typep)
{
    const WT_OPEN_CONF_KEY_MAP *key_map;

    key_map = __wt_open_conf_key_lookup(key_id);
    if (key_map == NULL)
        return (WT_NOTFOUND);

    if (key_namep != NULL)
        *key_namep = key_map->key_name;
    if (parent_namep != NULL)
        *parent_namep = key_map->parent_name;
    if (expected_typep != NULL)
        *expected_typep = key_map->expected_type;

    return (0);
}
