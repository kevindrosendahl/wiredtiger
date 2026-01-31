/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Test file for Phase 2: Verify struct config values are actually applied
 * and bypass string parsing.
 */

#include "test_util.h"
#include "wiredtiger_open_conf.h"

#include <math.h>

static const char *home;

/* Helper for floating point comparison with tolerance */
static bool
double_eq(double a, double b)
{
    return fabs(a - b) < 0.001;
}

/*
 * test_cache_size_applied --
 *     Verify cache_size from struct config is actually applied to the connection.
 */
static void
test_cache_size_applied(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    uint64_t expected_cache_size;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 50 * WT_MEGABYTE),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: cache_size value is applied\n");

    expected_cache_size = 50 * WT_MEGABYTE;

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    
    conn_impl = (WT_CONNECTION_IMPL *)conn;
    
    /* Verify the cache size was actually set to our value */
    testutil_assert(conn_impl->cache_size == expected_cache_size);
    
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED: cache_size = %lu\n", (unsigned long)expected_cache_size);
}

/*
 * test_cache_size_non_default --
 *     Verify a non-default cache_size is applied (proves we're not just getting defaults).
 */
static void
test_cache_size_non_default(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    uint64_t unusual_cache_size;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 73 * WT_MEGABYTE), /* Unusual value */
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: non-default cache_size is applied\n");

    unusual_cache_size = 73 * WT_MEGABYTE;

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    
    conn_impl = (WT_CONNECTION_IMPL *)conn;
    
    /* This proves we're not just getting default values */
    testutil_assert(conn_impl->cache_size == unusual_cache_size);
    
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED: cache_size = %lu (not default)\n", (unsigned long)unusual_cache_size);
}

/*
 * test_in_memory_flag_set --
 *     Verify in_memory flag is properly set on the connection.
 */
static void
test_in_memory_flag_set(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_in_memory, true),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: in_memory flag is set\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    
    conn_impl = (WT_CONNECTION_IMPL *)conn;
    
    /* Verify the in_memory flag was set */
    testutil_assert(F_ISSET(conn_impl, WT_CONN_IN_MEMORY));
    
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED: WT_CONN_IN_MEMORY flag is set\n");
}

/*
 * test_in_memory_flag_not_set --
 *     Verify in_memory=false means the flag is NOT set.
 */
static void
test_in_memory_flag_not_set(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_in_memory, false),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: in_memory=false means flag is NOT set\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    
    conn_impl = (WT_CONNECTION_IMPL *)conn;
    
    /* Verify the in_memory flag was NOT set */
    testutil_assert(!F_ISSET(conn_impl, WT_CONN_IN_MEMORY));
    
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED: WT_CONN_IN_MEMORY flag is NOT set\n");
}

/*
 * test_session_max_applied --
 *     Verify session_max from struct config is applied.
 */
static void
test_session_max_applied(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    uint32_t expected_session_max;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_session_max, 150),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: session_max value is applied\n");

    expected_session_max = 150;

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    
    conn_impl = (WT_CONNECTION_IMPL *)conn;
    
    /* Verify session_max was set (note: internal value may be adjusted) */
    /* The connection adds some internal sessions, so we check it's >= our value */
    testutil_assert(conn_impl->session_array.size >= expected_session_max);
    
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED: session_max >= %u\n", expected_session_max);
}

/*
 * test_eviction_target_applied --
 *     Verify eviction_target from struct config is applied.
 */
static void
test_eviction_target_applied(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    WT_EVICT *evict;
    double expected_target;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 100 * WT_MEGABYTE),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_target, 70),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: eviction_target value is applied\n");

    expected_target = 70.0;

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    
    conn_impl = (WT_CONNECTION_IMPL *)conn;
    evict = conn_impl->evict;
    
    /* Verify eviction_target was set */
    testutil_assert(double_eq(evict->eviction_target, expected_target));
    
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED: eviction_target = %.1f\n", expected_target);
}

/*
 * test_eviction_trigger_applied --
 *     Verify eviction_trigger from struct config is applied.
 */
static void
test_eviction_trigger_applied(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    WT_EVICT *evict;
    double expected_trigger;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 100 * WT_MEGABYTE),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_trigger, 90),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: eviction_trigger value is applied\n");

    expected_trigger = 90.0;

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    
    conn_impl = (WT_CONNECTION_IMPL *)conn;
    evict = conn_impl->evict;
    
    /* Verify eviction_trigger was set */
    testutil_assert(double_eq(evict->eviction_trigger, expected_trigger));
    
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED: eviction_trigger = %.1f\n", expected_trigger);
}

/*
 * test_multiple_values_applied --
 *     Verify multiple config values are all applied correctly.
 */
static void
test_multiple_values_applied(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    WT_EVICT *evict;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 64 * WT_MEGABYTE),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_target, 75),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_trigger, 95),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: multiple config values applied together\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    
    conn_impl = (WT_CONNECTION_IMPL *)conn;
    evict = conn_impl->evict;
    
    /* Verify all values were set correctly */
    testutil_assert(conn_impl->cache_size == 64 * WT_MEGABYTE);
    testutil_assert(double_eq(evict->eviction_target, 75.0));
    testutil_assert(double_eq(evict->eviction_trigger, 95.0));
    
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED: all values applied correctly\n");
}

/*
 * test_struct_config_stored --
 *     Verify the struct config is stored on the connection (needed for bypass).
 */
static void
test_struct_config_stored(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 100 * WT_MEGABYTE),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: struct config is stored on connection\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    
    conn_impl = (WT_CONNECTION_IMPL *)conn;
    
    /* Verify the conf_source was set and is struct type */
    testutil_assert(conn_impl->conf_source != NULL);
    testutil_assert(conn_impl->conf_source->type == WT_CONF_SOURCE_STRUCT);
    testutil_assert(conn_impl->conf_source->u.structured.args != NULL);
    
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED: conf_source stored on connection\n");
}

/*
 * test_read_from_struct_config --
 *     Verify values can be read directly from stored struct config (bypass demo).
 */
static void
test_read_from_struct_config(void)
{
    WT_CONNECTION *conn;
    WT_CONNECTION_IMPL *conn_impl;
    WT_CONF_SOURCE *source;
    WT_SESSION_IMPL *session;
    int64_t cache_val;
    bool in_memory_val;
    int ret;
    WT_OPEN_CONFIG_ARG config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 77 * WT_MEGABYTE),
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_in_memory, false),
        WT_OPEN_CONFIG_ARG_END
    };

    printf("Test: read values directly from stored struct config\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    
    conn_impl = (WT_CONNECTION_IMPL *)conn;
    session = conn_impl->default_session;
    source = conn_impl->conf_source;
    
    /* Read cache_size directly from struct config - no string parsing */
    ret = __wt_conf_source_get_int(session, source, WT_OPEN_CONF_cache_size, "cache_size", NULL, &cache_val);
    testutil_assert(ret == 0);
    testutil_assert(cache_val == 77 * WT_MEGABYTE);
    
    /* Read in_memory directly from struct config - no string parsing */
    ret = __wt_conf_source_get_boolean(session, source, WT_OPEN_CONF_in_memory, "in_memory", NULL, &in_memory_val);
    testutil_assert(ret == 0);
    testutil_assert(in_memory_val == false);
    
    /* Try reading a key that wasn't provided - should return WT_NOTFOUND */
    ret = __wt_conf_source_get_int(session, source, WT_OPEN_CONF_session_max, "session_max", NULL, &cache_val);
    testutil_assert(ret == WT_NOTFOUND);
    
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED: read from struct config bypasses string parsing\n");
}

/*
 * test_comparison_cache_size --
 *     Verify struct config produces identical cache_size as string config.
 */
static void
test_comparison_cache_size(void)
{
    WT_CONNECTION *conn_string, *conn_struct;
    WT_CONNECTION_IMPL *impl_string, *impl_struct;
    uint64_t string_cache_size, struct_cache_size;

    printf("Test: comparison - struct and string config produce identical cache_size\n");

    /* Open with string config */
    testutil_check(wiredtiger_open(home, NULL, "create,cache_size=67108864", &conn_string));
    impl_string = (WT_CONNECTION_IMPL *)conn_string;

    /* Save the value, close */
    string_cache_size = impl_string->cache_size;
    testutil_check(conn_string->close(conn_string, NULL));

    /* Recreate directory for clean state */
    testutil_recreate_dir(home);

    /* Open with struct config - same value */
    WT_OPEN_CONFIG_ARG struct_config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 67108864),
        WT_OPEN_CONFIG_ARG_END
    };
    testutil_check(wiredtiger_open_ex(home, NULL, struct_config, 0, &conn_struct));
    impl_struct = (WT_CONNECTION_IMPL *)conn_struct;

    /* Compare */
    struct_cache_size = impl_struct->cache_size;
    testutil_assert(struct_cache_size == string_cache_size);
    testutil_check(conn_struct->close(conn_struct, NULL));

    printf("  PASSED: cache_size identical (%" PRIu64 ")\n", string_cache_size);
}

/*
 * test_comparison_eviction_targets --
 *     Verify struct config produces identical eviction targets as string config.
 */
static void
test_comparison_eviction_targets(void)
{
    WT_CONNECTION *conn_string, *conn_struct;
    WT_CONNECTION_IMPL *impl_string, *impl_struct;
    WT_EVICT *evict_string, *evict_struct;
    double string_target, string_trigger, string_dirty_target, string_dirty_trigger;

    printf("Test: comparison - struct and string config produce identical eviction targets\n");

    /* Open with string config */
    testutil_check(wiredtiger_open(home, NULL,
      "create,eviction_target=75,eviction_trigger=92,eviction_dirty_target=12,eviction_dirty_trigger=18",
      &conn_string));
    impl_string = (WT_CONNECTION_IMPL *)conn_string;
    evict_string = impl_string->evict;

    /* Save values */
    string_target = evict_string->eviction_target;
    string_trigger = evict_string->eviction_trigger;
    string_dirty_target = evict_string->eviction_dirty_target;
    string_dirty_trigger = evict_string->eviction_dirty_trigger;
    testutil_check(conn_string->close(conn_string, NULL));

    /* Recreate directory */
    testutil_recreate_dir(home);

    /* Open with struct config - same values */
    WT_OPEN_CONFIG_ARG struct_config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_target, 75),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_trigger, 92),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_dirty_target, 12),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_dirty_trigger, 18),
        WT_OPEN_CONFIG_ARG_END
    };
    testutil_check(wiredtiger_open_ex(home, NULL, struct_config, 0, &conn_struct));
    impl_struct = (WT_CONNECTION_IMPL *)conn_struct;
    evict_struct = impl_struct->evict;

    /* Compare - all should be identical */
    testutil_assert(double_eq(evict_struct->eviction_target, string_target));
    testutil_assert(double_eq(evict_struct->eviction_trigger, string_trigger));
    testutil_assert(double_eq(evict_struct->eviction_dirty_target, string_dirty_target));
    testutil_assert(double_eq(evict_struct->eviction_dirty_trigger, string_dirty_trigger));
    testutil_check(conn_struct->close(conn_struct, NULL));

    printf("  PASSED: all eviction values identical\n");
}

/*
 * test_comparison_session_max --
 *     Verify struct config produces identical session_max as string config.
 */
static void
test_comparison_session_max(void)
{
    WT_CONNECTION *conn_string, *conn_struct;
    WT_CONNECTION_IMPL *impl_string, *impl_struct;
    uint32_t string_session_array_size, struct_session_array_size;

    printf("Test: comparison - struct and string config produce identical session_max\n");

    /* Open with string config */
    testutil_check(wiredtiger_open(home, NULL, "create,session_max=200", &conn_string));
    impl_string = (WT_CONNECTION_IMPL *)conn_string;

    string_session_array_size = impl_string->session_array.size;
    testutil_check(conn_string->close(conn_string, NULL));

    testutil_recreate_dir(home);

    /* Open with struct config */
    WT_OPEN_CONFIG_ARG struct_config[] = {
        WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
        WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_session_max, 200),
        WT_OPEN_CONFIG_ARG_END
    };
    testutil_check(wiredtiger_open_ex(home, NULL, struct_config, 0, &conn_struct));
    impl_struct = (WT_CONNECTION_IMPL *)conn_struct;

    /* Session array size should be identical */
    struct_session_array_size = impl_struct->session_array.size;
    testutil_assert(struct_session_array_size == string_session_array_size);
    testutil_check(conn_struct->close(conn_struct, NULL));

    printf("  PASSED: session_array.size identical (%u)\n", string_session_array_size);
}

int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    home = opts->home;

    printf("=== Phase 2 Tests: Verify Config Values Are Applied ===\n\n");

    testutil_recreate_dir(home);
    test_cache_size_applied();

    testutil_recreate_dir(home);
    test_cache_size_non_default();

    testutil_recreate_dir(home);
    test_in_memory_flag_set();

    testutil_recreate_dir(home);
    test_in_memory_flag_not_set();

    testutil_recreate_dir(home);
    test_session_max_applied();

    testutil_recreate_dir(home);
    test_eviction_target_applied();

    testutil_recreate_dir(home);
    test_eviction_trigger_applied();

    testutil_recreate_dir(home);
    test_multiple_values_applied();

    testutil_recreate_dir(home);
    test_struct_config_stored();

    testutil_recreate_dir(home);
    test_read_from_struct_config();

    /* Comparison tests - verify struct and string config produce identical results */
    testutil_recreate_dir(home);
    test_comparison_cache_size();

    testutil_recreate_dir(home);
    test_comparison_eviction_targets();

    testutil_recreate_dir(home);
    test_comparison_session_max();

    printf("\n=== All Phase 2 Tests PASSED ===\n");

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
