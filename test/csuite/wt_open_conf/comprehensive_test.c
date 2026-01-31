/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 */
#include "test_util.h"
#include "wiredtiger_open_conf.h"

/*
 * Comprehensive tests for wiredtiger_open_ex API.
 * Tests all config keys with bypass verification.
 */

static const char *home;

/*
 * test_file_manager_config --
 *     Test file_manager configuration options.
 */
static void
test_file_manager_config(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 50 * WT_MEGABYTE),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_file_manager_close_idle_time, 45),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_file_manager_close_scan_interval, 15),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_file_manager_close_handle_minimum, 300),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: file_manager configuration\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    conn_impl = (WT_CONNECTION_IMPL *)conn;

    testutil_assert(conn_impl->sweep_idle_time == 45);
    testutil_assert(conn_impl->sweep_interval == 15);
    testutil_assert(conn_impl->sweep_handles_min == 300);

    testutil_check(conn->close(conn, NULL));
    printf("  PASSED\n");
}

/*
 * test_checkpoint_config --
 *     Test checkpoint configuration options.
 */
static void
test_checkpoint_config(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 50 * WT_MEGABYTE),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_checkpoint_wait, 120),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_checkpoint_log_size, 5LL * WT_GIGABYTE),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: checkpoint configuration\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    conn_impl = (WT_CONNECTION_IMPL *)conn;

    /* checkpoint.wait is stored as microseconds */
    testutil_assert(conn_impl->ckpt.server.usecs == 120ULL * WT_MILLION);

    testutil_check(conn->close(conn, NULL));
    printf("  PASSED\n");
}

/*
 * test_eviction_threads_config --
 *     Test eviction threads configuration.
 */
static void
test_eviction_threads_config(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 50 * WT_MEGABYTE),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_threads_min, 2),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_threads_max, 6),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: eviction threads configuration\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    conn_impl = (WT_CONNECTION_IMPL *)conn;

    testutil_assert(conn_impl->evict_threads_min == 2);
    testutil_assert(conn_impl->evict_threads_max == 6);

    testutil_check(conn->close(conn, NULL));
    printf("  PASSED\n");
}

/*
 * test_cache_timeout_config --
 *     Test cache timeout configuration.
 */
static void
test_cache_timeout_config(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 50 * WT_MEGABYTE),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_max_wait_ms, 5000),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_stuck_timeout_ms, 60000),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: cache timeout configuration\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    conn_impl = (WT_CONNECTION_IMPL *)conn;

    /* cache_max_wait_ms is stored as microseconds */
    testutil_assert(conn_impl->evict->cache_max_wait_us == 5000ULL * WT_THOUSAND);
    testutil_assert(conn_impl->evict->cache_stuck_timeout_ms == 60000);

    testutil_check(conn->close(conn, NULL));
    printf("  PASSED\n");
}

/*
 * test_session_max_config --
 *     Test session_max configuration.
 */
static void
test_session_max_config(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 50 * WT_MEGABYTE),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_session_max, 200),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: session_max configuration\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    conn_impl = (WT_CONNECTION_IMPL *)conn;

    /* session_max is used to size the session array, value should be >= 200 + internal sessions */
    testutil_assert(conn_impl->session_array.size >= 200);

    testutil_check(conn->close(conn, NULL));
    printf("  PASSED\n");
}

/*
 * test_log_config_full --
 *     Test full log configuration.
 */
static void
test_log_config_full(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    char log_dir[256];

    testutil_snprintf(log_dir, sizeof(log_dir), "%s/journal", home);
    testutil_recreate_dir(log_dir);

    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 50 * WT_MEGABYTE),
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_log_enabled, true),
        WT_OPEN_CONFIG_ARG_SET_STR(WT_OPEN_CONF_log_path, "journal", 7),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_log_file_max, 50 * WT_MEGABYTE),
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_log_remove, true),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: full log configuration\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    conn_impl = (WT_CONNECTION_IMPL *)conn;

    testutil_assert(F_ISSET(&conn_impl->log_mgr, WT_LOG_CONFIG_ENABLED));
    testutil_assert(conn_impl->log_mgr.file_max == 50 * WT_MEGABYTE);
    testutil_assert(F_ISSET(&conn_impl->log_mgr, WT_LOG_REMOVE));

    testutil_check(conn->close(conn, NULL));
    printf("  PASSED\n");
}

/*
 * test_combined_config --
 *     Test multiple configuration options together.
 */
static void
test_combined_config(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 100 * WT_MEGABYTE),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_session_max, 150),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_target, 75),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_trigger, 92),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_threads_min, 3),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_threads_max, 8),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_file_manager_close_idle_time, 60),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_checkpoint_wait, 90),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: combined configuration\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    conn_impl = (WT_CONNECTION_IMPL *)conn;

    testutil_assert(conn_impl->cache_size == 100ULL * WT_MEGABYTE);
    testutil_assert(conn_impl->session_array.size >= 150);
    testutil_assert(conn_impl->evict_threads_min == 3);
    testutil_assert(conn_impl->evict_threads_max == 8);
    testutil_assert(conn_impl->sweep_idle_time == 60);
    testutil_assert(conn_impl->ckpt.server.usecs == 90ULL * WT_MILLION);

    testutil_check(conn->close(conn, NULL));
    printf("  PASSED\n");
}

/*
 * test_config_base --
 *     Test config_base option.
 */
static void
test_config_base(void)
{
    WT_CONNECTION *conn;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 50 * WT_MEGABYTE),
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_config_base, false),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: config_base=false configuration\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    home = opts->home;

    printf("=== Comprehensive Tests: All Configuration Keys ===\n\n");

    /* Clean up from any previous run */
    testutil_recreate_dir(home);

    test_file_manager_config();
    testutil_recreate_dir(home);

    test_checkpoint_config();
    testutil_recreate_dir(home);

    test_eviction_threads_config();
    testutil_recreate_dir(home);

    test_cache_timeout_config();
    testutil_recreate_dir(home);

    test_session_max_config();
    testutil_recreate_dir(home);

    test_log_config_full();
    testutil_recreate_dir(home);

    test_combined_config();
    testutil_recreate_dir(home);

    test_config_base();

    printf("\n=== All Comprehensive Tests PASSED ===\n");

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
