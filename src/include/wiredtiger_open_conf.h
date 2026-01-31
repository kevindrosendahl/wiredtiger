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

#ifndef __WIREDTIGER_OPEN_CONF_H_
#define __WIREDTIGER_OPEN_CONF_H_

/*
 * Public configuration key IDs for use with wiredtiger_open_ex.
 *
 * NAMING: These use WT_OPEN_CONF_* prefix (not WT_CONF_ID_*) to clearly
 * distinguish from the internal WT_CONF_ID_* identifiers in conf_keys.h.
 * The internal IDs use bit-packing for nested keys; these are flat and stable.
 *
 * ABI STABILITY POLICY:
 * - Key ID values are explicit and will NEVER change between versions
 * - New options get new IDs; existing IDs are NEVER removed or reused
 * - Deprecated keys: ID is retained forever, usage produces a warning,
 *   behavior may become a no-op
 * - Unknown keys: wiredtiger_open_ex returns EINVAL with descriptive message
 *
 * Key ranges:
 *   1000-1099: Core options
 *   1100-1199: Eviction options
 *   1200-1299: Log options
 *   1300-1399: Checkpoint options
 *   1400-1499: Statistics options
 *   1500-1599: File Manager options
 *   1600-1699: Debug Mode options
 *   1700-1799: Compatibility options
 *   1800-1899: Prefetch options
 *   1900-1999: Live Restore options
 *   2000-2099: Encryption options
 *   2100-2199: Chunk Cache options
 *   2200-2299: Tiered options
 *   2300-2399: Hash options
 *   2400-2499: Transaction Sync options
 *   2500-2599: Io Capacity options
 *   2600-2699: History Store options
 *   2700-2799: Shared Cache options
 *   2800-2899: Block Cache options
 */

/*
 * Sentinel value for array terminator.
 * Using UINT64_MAX is safer than 0 since 0 could theoretically be a valid key.
 */
#ifndef WT_OPEN_CONF_KEY_END
#define WT_OPEN_CONF_KEY_END UINT64_MAX
#endif

/* ============================================================================
 * Core options (1000-1099)
 * ============================================================================ */
#define WT_OPEN_CONF_cache_size                         1001  /* int: maximum heap memory to allocate for the cache. ... */
#define WT_OPEN_CONF_cache_overhead                     1002  /* int: assume the heap allocator overhead is the speci... */
#define WT_OPEN_CONF_cache_max_wait_ms                  1003  /* int: the maximum number of milliseconds an applicati... */
#define WT_OPEN_CONF_cache_stuck_timeout_ms             1004  /* int: the number of milliseconds to wait before a stu... */
#define WT_OPEN_CONF_checkpoint_sync                    1005  /* bool: flush files to stable storage when closing or w... */
#define WT_OPEN_CONF_create                             1006  /* bool: create the database if it does not exist */
#define WT_OPEN_CONF_error_prefix                       1007  /* string: prefix string for error messages */
#define WT_OPEN_CONF_in_memory                          1008  /* bool: keep data in memory only. See @ref in_memory fo... */
#define WT_OPEN_CONF_mmap                               1009  /* bool: Use memory mapping when accessing files in a re... */
#define WT_OPEN_CONF_mmap_all                           1010  /* bool: Use memory mapping to read and write all data f... */
#define WT_OPEN_CONF_readonly                           1011  /* bool: open connection in read-only mode. The database... */
#define WT_OPEN_CONF_salvage                            1012  /* bool: open connection and salvage any WiredTiger-owne... */
#define WT_OPEN_CONF_session_max                        1013  /* int: maximum expected number of sessions (including ... */
#define WT_OPEN_CONF_session_scratch_max                1014  /* int: maximum memory to cache in each session */
#define WT_OPEN_CONF_config_base                        1015  /* bool: write the base configuration file if creating t... */
#define WT_OPEN_CONF_cache_cursors                      1016  /* bool: enable caching of cursors for reuse. This is th... */
#define WT_OPEN_CONF_json_output                        1017  /* string (list): enable JSON formatted messages on the event han... */
#define WT_OPEN_CONF_verbose                            1018  /* string (list): enable messages for various subsystems and oper... */
#define WT_OPEN_CONF_operation_timeout_ms               1019  /* int: this option is no longer supported, retained fo... */
#define WT_OPEN_CONF_generation_drain_timeout_ms        1020  /* int: the number of milliseconds to wait for a resour... */
#define WT_OPEN_CONF_timing_stress_for_test             1021  /* string (list): enable code that interrupts the usual timing of... */
#define WT_OPEN_CONF_extra_diagnostics                  1022  /* string (list): enable additional diagnostics in WiredTiger. Th... */
#define WT_OPEN_CONF_backup_restore_target              1023  /* string (list): If non-empty and restoring from a backup, resto... */
#define WT_OPEN_CONF_statistics                         1024  /* string (list): Maintain database statistics, which may impact ... */
#define WT_OPEN_CONF_eviction_target                    1025  /* int: perform eviction in worker threads when the cac... */
#define WT_OPEN_CONF_eviction_trigger                   1026  /* int: trigger application threads to perform eviction... */
#define WT_OPEN_CONF_eviction_dirty_target              1027  /* int: perform eviction in worker threads when the cac... */
#define WT_OPEN_CONF_eviction_dirty_trigger             1028  /* int: trigger application threads to perform eviction... */
#define WT_OPEN_CONF_eviction_updates_target            1029  /* int: perform eviction in worker threads when the cac... */
#define WT_OPEN_CONF_eviction_updates_trigger           1030  /* int: trigger application threads to perform eviction... */
#define WT_OPEN_CONF_eviction_checkpoint_target         1031  /* int: perform eviction at the beginning of checkpoint... */
#define WT_OPEN_CONF_buffer_alignment                   1032  /* int: this option is no longer supported, retained fo... */
#define WT_OPEN_CONF_builtin_extension_config           1033  /* string: A structure where the keys are the names of bui... */
#define WT_OPEN_CONF_compile_configuration_count        1034  /* int: the number of configuration strings that can be... */
#define WT_OPEN_CONF_direct_io                          1035  /* string (list): this option is no longer supported, retained fo... */
#define WT_OPEN_CONF_disaggregated_checkpoint_meta      1036  /* string: the checkpoint metadata from which to start (or... */
#define WT_OPEN_CONF_disaggregated_drain_threads        1037  /* int: The number of threads used to drain the ingest ... */
#define WT_OPEN_CONF_disaggregated_last_materialized_lsn 1038  /* int: the page LSN indicating that all pages up until... */
#define WT_OPEN_CONF_disaggregated_local_files_action   1039  /* string: what should be done to the local files in disag... */
#define WT_OPEN_CONF_disaggregated_lose_all_my_data     1040  /* bool: This setting skips file system syncs, and will ... */
#define WT_OPEN_CONF_disaggregated_role                 1041  /* string: whether the stable table in a layered data stor... */
#define WT_OPEN_CONF_disaggregated_page_log             1042  /* string: The page log service used as a backing for this... */
#define WT_OPEN_CONF_exclusive                          1043  /* bool: fail if the database already exists, generally ... */
#define WT_OPEN_CONF_extensions                         1044  /* string (list): list of shared library extensions to load (usin... */
#define WT_OPEN_CONF_file_extend                        1045  /* string (list): file size extension configuration. If set, exte... */
#define WT_OPEN_CONF_hazard_max                         1046  /* int: maximum number of simultaneous hazard pointers ... */
#define WT_OPEN_CONF_heuristic_controls_checkpoint_cleanup_obsolete_tw_pages_dirty_max 1047  /* int: maximum number of obsolete time window pages th... */
#define WT_OPEN_CONF_heuristic_controls_eviction_obsolete_tw_pages_dirty_max 1048  /* int: maximum number of obsolete time window pages th... */
#define WT_OPEN_CONF_heuristic_controls_obsolete_tw_btree_max 1049  /* int: maximum number of btrees that can be checked fo... */
#define WT_OPEN_CONF_multiprocess                       1050  /* bool: permit sharing between processes (will automati... */
#define WT_OPEN_CONF_operation_tracking_enabled         1051  /* bool: enable operation tracking subsystem */
#define WT_OPEN_CONF_operation_tracking_path            1052  /* string: the name of a directory into which operation tr... */
#define WT_OPEN_CONF_page_delta_delta_pct               1053  /* int: the size threshold (as a percentage) at which a... */
#define WT_OPEN_CONF_page_delta_internal_page_delta     1054  /* bool: When enabled, reconciliation may write deltas f... */
#define WT_OPEN_CONF_page_delta_leaf_page_delta         1055  /* bool: When enabled, reconciliation may write deltas f... */
#define WT_OPEN_CONF_page_delta_max_consecutive_delta   1056  /* int: the max consecutive deltas allowed for a single... */
#define WT_OPEN_CONF_precise_checkpoint                 1057  /* bool: Only write data with timestamps that are smalle... */
#define WT_OPEN_CONF_preserve_prepared                  1058  /* bool: open connection in preserve prepare mode. All t... */
#define WT_OPEN_CONF_rollback_to_stable_threads         1059  /* int: maximum number of threads WiredTiger will start... */
#define WT_OPEN_CONF_session_table_cache                1060  /* bool: Maintain a per-session cache of tables */
#define WT_OPEN_CONF_use_environment                    1061  /* bool: use the \c WIREDTIGER_CONFIG and \c WIREDTIGER_... */
#define WT_OPEN_CONF_use_environment_priv               1062  /* bool: use the \c WIREDTIGER_CONFIG and \c WIREDTIGER_... */
#define WT_OPEN_CONF_verify_metadata                    1063  /* bool: open connection and verify any WiredTiger metad... */
#define WT_OPEN_CONF_write_through                      1064  /* string (list): Use \c FILE_FLAG_WRITE_THROUGH on Windows to wr... */

/* ============================================================================
 * Eviction options (1100-1199)
 * ============================================================================ */
#define WT_OPEN_CONF_eviction_threads_min               1107  /* int: minimum number of threads WiredTiger will start... */
#define WT_OPEN_CONF_eviction_threads_max               1108  /* int: maximum number of threads WiredTiger will start... */
#define WT_OPEN_CONF_eviction_evict_sample_inmem        1109  /* bool: If no in-memory ref is found on the root page, ... */
#define WT_OPEN_CONF_eviction_evict_use_softptr         1110  /* bool: Experimental: Use "soft pointers" instead of ha... */
#define WT_OPEN_CONF_eviction_legacy_page_visit_strategy 1111  /* bool: Use legacy page visit strategy for eviction. Us... */
#define WT_OPEN_CONF_eviction_app_eviction_min_cache_fill_ratio 1112  /* int: This setting establishes a minimum cache fill r... */
#define WT_OPEN_CONF_eviction_cache_tolerance_for_app_eviction 1113  /* int: This setting establishes a tolerance level for ... */
#define WT_OPEN_CONF_eviction_incremental_app_eviction  1114  /* bool: Only a part of application threads will partici... */
#define WT_OPEN_CONF_eviction_prefer_scrub_eviction     1115  /* bool: Change the eviction strategy to scrub eviction ... */
#define WT_OPEN_CONF_eviction_skip_update_obsolete_check 1116  /* bool: Skip checking for obsolete updates whenever an ... */

/* ============================================================================
 * Log options (1200-1299)
 * ============================================================================ */
#define WT_OPEN_CONF_log_enabled                        1200  /* bool: enable logging subsystem */
#define WT_OPEN_CONF_log_archive                        1201  /* bool: automatically remove unneeded log files (deprec... */
#define WT_OPEN_CONF_log_path                           1202  /* string: the name of a directory into which log files ar... */
#define WT_OPEN_CONF_log_file_max                       1203  /* int: the maximum size of log files */
#define WT_OPEN_CONF_log_prealloc                       1204  /* bool: pre-allocate log files */
#define WT_OPEN_CONF_log_zero_fill                      1205  /* bool: manually write zeroes into log files */
#define WT_OPEN_CONF_log_compressor                     1206  /* string: configure a compressor for log records. Permitt... */
#define WT_OPEN_CONF_log_remove                         1207  /* bool: automatically remove unneeded log files */
#define WT_OPEN_CONF_log_recover                        1208  /* string: run recovery or fail with an error if recovery ... */
#define WT_OPEN_CONF_log_os_cache_dirty_pct             1209  /* int: maximum dirty system buffer cache usage, as a p... */
#define WT_OPEN_CONF_log_prealloc_init_count            1210  /* int: initial number of pre-allocated log files */
#define WT_OPEN_CONF_log_force_write_wait               1211  /* int: enable code that interrupts the usual timing of... */
#define WT_OPEN_CONF_log_recovery_skip                  1212  /* bool: if enabled, skip log scanning on startup after ... */

/* ============================================================================
 * Checkpoint options (1300-1399)
 * ============================================================================ */
#define WT_OPEN_CONF_checkpoint_wait                    1300  /* int: seconds to wait between each checkpoint; settin... */
#define WT_OPEN_CONF_checkpoint_log_size                1301  /* int: wait for this amount of log record bytes to be ... */
#define WT_OPEN_CONF_checkpoint_cleanup_wait            1302  /* int: seconds to wait between each checkpoint cleanup */
#define WT_OPEN_CONF_checkpoint_cleanup_method          1303  /* string: control how aggressively obsolete content is re... */
#define WT_OPEN_CONF_checkpoint_cleanup_file_wait_ms    1304  /* int: the number of milliseconds to wait between each... */

/* ============================================================================
 * Statistics options (1400-1499)
 * ============================================================================ */
#define WT_OPEN_CONF_statistics_log_wait                1401  /* int: seconds to wait between each write of the log r... */
#define WT_OPEN_CONF_statistics_log_on_close            1402  /* bool: log statistics on database close */
#define WT_OPEN_CONF_statistics_log_path                1403  /* string: the name of a directory into which statistics f... */
#define WT_OPEN_CONF_statistics_log_sources             1404  /* string (list): if non-empty, include statistics for the list o... */
#define WT_OPEN_CONF_statistics_log_timestamp           1405  /* string: a timestamp prepended to each log record. May c... */
#define WT_OPEN_CONF_statistics_log_json                1406  /* bool: encode statistics in JSON format */

/* ============================================================================
 * File Manager options (1500-1599)
 * ============================================================================ */
#define WT_OPEN_CONF_file_manager_close_idle_time       1500  /* int: amount of time in seconds a file handle needs t... */
#define WT_OPEN_CONF_file_manager_close_scan_interval   1501  /* int: interval in seconds at which to check for files... */
#define WT_OPEN_CONF_file_manager_close_handle_minimum  1502  /* int: number of handles open before the file manager ... */

/* ============================================================================
 * Debug Mode options (1600-1699)
 * ============================================================================ */
#define WT_OPEN_CONF_debug_mode_corruption_abort        1600  /* bool: if true and built in diagnostic mode, dump core... */
#define WT_OPEN_CONF_debug_mode_eviction                1601  /* bool: if true, modify internal algorithms to change s... */
#define WT_OPEN_CONF_debug_mode_cursor_copy             1602  /* bool: if true, use the system allocator to make a cop... */
#define WT_OPEN_CONF_debug_mode_table_logging           1603  /* bool: if true, write transaction related information ... */
#define WT_OPEN_CONF_debug_mode_checkpoint_retention    1604  /* int: adjust log removal to retain the log records of... */
#define WT_OPEN_CONF_debug_mode_configuration           1605  /* bool: if true, display invalid cache configuration wa... */
#define WT_OPEN_CONF_debug_mode_background_compact      1606  /* bool: if true, background compact aggressively remove... */
#define WT_OPEN_CONF_debug_mode_crash_point_colgroup    1607  /* bool: if true, force crash in table creation while cr... */
#define WT_OPEN_CONF_debug_mode_cursor_reposition       1608  /* bool: if true, for operations with snapshot isolation... */
#define WT_OPEN_CONF_debug_mode_disagg_address_cookie_upgrade 1609  /* string: modify the disaggregated block manager to prete... */
#define WT_OPEN_CONF_debug_mode_disagg_address_cookie_optional_field 1610  /* bool: if true, modify the disaggregated block manager... */
#define WT_OPEN_CONF_debug_mode_log_retention           1611  /* int: adjust log removal to retain at least this numb... */
#define WT_OPEN_CONF_debug_mode_page_history            1612  /* bool: if true, keep track of per-page usage statistic... */
#define WT_OPEN_CONF_debug_mode_realloc_exact           1613  /* bool: if true, reallocation of memory will only provi... */
#define WT_OPEN_CONF_debug_mode_realloc_malloc          1614  /* bool: if true, every realloc call will force a new me... */
#define WT_OPEN_CONF_debug_mode_rollback_error          1615  /* int: return a WT_ROLLBACK error from a transaction o... */
#define WT_OPEN_CONF_debug_mode_slow_checkpoint         1616  /* bool: if true, slow down checkpoint creation by slowi... */
#define WT_OPEN_CONF_debug_mode_stress_skiplist         1617  /* bool: Configure various internal parameters to encour... */
#define WT_OPEN_CONF_debug_mode_tiered_flush_error_continue 1618  /* bool: on a write to tiered storage, continue when an ... */
#define WT_OPEN_CONF_debug_mode_update_restore_evict    1619  /* bool: if true, control all dirty page evictions throu... */
#define WT_OPEN_CONF_debug_mode_eviction_checkpoint_ts_ordering 1620  /* bool: if true, act as if eviction is being run in par... */

/* ============================================================================
 * Compatibility options (1700-1799)
 * ============================================================================ */
#define WT_OPEN_CONF_compatibility_require_min          1700  /* string: required minimum compatibility version of exist... */
#define WT_OPEN_CONF_compatibility_require_max          1701  /* string: required maximum compatibility version of exist... */
#define WT_OPEN_CONF_compatibility_release              1702  /* string: compatibility release version string */

/* ============================================================================
 * Prefetch options (1800-1899)
 * ============================================================================ */
#define WT_OPEN_CONF_prefetch_available                 1800  /* bool: whether the thread pool for the pre-fetch funct... */
#define WT_OPEN_CONF_prefetch_default                   1801  /* bool: whether pre-fetch is enabled for all sessions b... */

/* ============================================================================
 * Live Restore options (1900-1999)
 * ============================================================================ */
#define WT_OPEN_CONF_live_restore_enabled               1900  /* bool: whether live restore is enabled or not. */
#define WT_OPEN_CONF_live_restore_path                  1901  /* string: the path to the backup that will be restored from. */
#define WT_OPEN_CONF_live_restore_threads_max           1902  /* int: maximum number of threads WiredTiger will start... */
#define WT_OPEN_CONF_live_restore_read_size             1903  /* int: the read size for data migration, in bytes, mus... */

/* ============================================================================
 * Encryption options (2000-2099)
 * ============================================================================ */
#define WT_OPEN_CONF_encryption_name                    2000  /* string: Permitted values are \c "none" or a custom encr... */
#define WT_OPEN_CONF_encryption_keyid                   2001  /* string: An identifier that identifies a unique instance... */
#define WT_OPEN_CONF_encryption_secretkey               2002  /* string: A string that is passed to the WT_ENCRYPTOR::cu... */

/* ============================================================================
 * Chunk Cache options (2100-2199)
 * ============================================================================ */
#define WT_OPEN_CONF_chunk_cache_pinned                 2100  /* string (list): List of "table:" URIs exempt from cache evictio... */
#define WT_OPEN_CONF_chunk_cache_capacity               2101  /* int: maximum memory or storage to use for the chunk ... */
#define WT_OPEN_CONF_chunk_cache_chunk_cache_evict_trigger 2102  /* int: chunk cache percent full that triggers eviction */
#define WT_OPEN_CONF_chunk_cache_chunk_size             2103  /* int: size of cached chunks */
#define WT_OPEN_CONF_chunk_cache_storage_path           2104  /* string: the path (absolute or relative) to the file use... */
#define WT_OPEN_CONF_chunk_cache_enabled                2105  /* bool: enable chunk cache */
#define WT_OPEN_CONF_chunk_cache_hashsize               2106  /* int: number of buckets in the hashtable that keeps t... */
#define WT_OPEN_CONF_chunk_cache_flushed_data_cache_insertion 2107  /* bool: enable caching of freshly-flushed data, before ... */
#define WT_OPEN_CONF_chunk_cache_type                   2108  /* string: cache location, defaults to the file system. */

/* ============================================================================
 * Tiered options (2200-2299)
 * ============================================================================ */
#define WT_OPEN_CONF_tiered_storage_local_retention     2200  /* int: time in seconds to retain data on tiered storag... */
#define WT_OPEN_CONF_tiered_storage_auth_token          2201  /* string: authentication string identifier */
#define WT_OPEN_CONF_tiered_storage_bucket              2202  /* string: bucket string identifier where the objects shou... */
#define WT_OPEN_CONF_tiered_storage_bucket_prefix       2203  /* string: unique string prefix to identify our objects in... */
#define WT_OPEN_CONF_tiered_storage_cache_directory     2204  /* string: a directory to store locally cached versions of... */
#define WT_OPEN_CONF_tiered_storage_interval            2205  /* int: interval in seconds at which to check for tiere... */
#define WT_OPEN_CONF_tiered_storage_name                2206  /* string: Permitted values are \c "none" or a custom stor... */
#define WT_OPEN_CONF_tiered_storage_shared              2207  /* bool: enable sharing tiered tables across other Wired... */

/* ============================================================================
 * Hash options (2300-2399)
 * ============================================================================ */
#define WT_OPEN_CONF_hash_buckets                       2300  /* int: configure the number of hash buckets for most s... */
#define WT_OPEN_CONF_hash_dhandle_buckets               2301  /* int: configure the number of hash buckets for hash a... */

/* ============================================================================
 * Transaction Sync options (2400-2499)
 * ============================================================================ */
#define WT_OPEN_CONF_transaction_sync_enabled           2400  /* bool: whether to sync the log on every commit by defa... */
#define WT_OPEN_CONF_transaction_sync_method            2401  /* string: the method used to ensure log records are stabl... */

/* ============================================================================
 * Io Capacity options (2500-2599)
 * ============================================================================ */
#define WT_OPEN_CONF_io_capacity_total                  2500  /* int: number of bytes per second available to all sub... */
#define WT_OPEN_CONF_io_capacity_chunk_cache            2501  /* int: number of bytes per second available to the chu... */

/* ============================================================================
 * History Store options (2600-2699)
 * ============================================================================ */
#define WT_OPEN_CONF_history_store_file_max             2600  /* int: the maximum number of bytes that WiredTiger is ... */

/* ============================================================================
 * Shared Cache options (2700-2799)
 * ============================================================================ */
#define WT_OPEN_CONF_shared_cache_chunk                 2700  /* int: the granularity that a shared cache is redistri... */
#define WT_OPEN_CONF_shared_cache_name                  2701  /* string: the name of a cache that is shared between data... */
#define WT_OPEN_CONF_shared_cache_quota                 2702  /* int: maximum size of cache this database can be allo... */
#define WT_OPEN_CONF_shared_cache_reserve               2703  /* int: amount of cache this database is guaranteed to ... */
#define WT_OPEN_CONF_shared_cache_size                  2704  /* int: maximum memory to allocate for the shared cache... */

/* ============================================================================
 * Block Cache options (2800-2899)
 * ============================================================================ */
#define WT_OPEN_CONF_block_cache_cache_on_checkpoint    2800  /* bool: cache blocks written by a checkpoint */
#define WT_OPEN_CONF_block_cache_cache_on_writes        2801  /* bool: cache blocks as they are written (other than ch... */
#define WT_OPEN_CONF_block_cache_enabled                2802  /* bool: enable block cache */
#define WT_OPEN_CONF_block_cache_blkcache_eviction_aggression 2803  /* int: seconds an unused block remains in the cache be... */
#define WT_OPEN_CONF_block_cache_full_target            2804  /* int: the fraction of the block cache that must be fu... */
#define WT_OPEN_CONF_block_cache_size                   2805  /* int: maximum memory to allocate for the block cache */
#define WT_OPEN_CONF_block_cache_hashsize               2806  /* int: number of buckets in the hashtable that keeps t... */
#define WT_OPEN_CONF_block_cache_max_percent_overhead   2807  /* int: maximum tolerated overhead expressed as the num... */
#define WT_OPEN_CONF_block_cache_nvram_path             2808  /* string: the absolute path to the file system mounted on... */
#define WT_OPEN_CONF_block_cache_percent_file_in_dram   2809  /* int: bypass cache for a file if the set percentage o... */
#define WT_OPEN_CONF_block_cache_system_ram             2810  /* int: the bytes of system DRAM available for caching ... */
#define WT_OPEN_CONF_block_cache_type                   2811  /* string: cache location: DRAM or NVRAM */

#endif /* __WIREDTIGER_OPEN_CONF_H_ */
