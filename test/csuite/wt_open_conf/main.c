/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "test_util.h"
#include "wiredtiger_open_conf.h"

/*
 * Test program for wiredtiger_open_ex API.
 *
 * This tests the structured configuration API for wiredtiger_open.
 */

static const char *home;

/*
 * test_basic_open --
 *     Test basic open with struct config.
 */
static void
test_basic_open(void)
{
    WT_CONNECTION *conn;
    WT_OPEN_CONFIG_ARG config[] = {WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 100 * WT_MEGABYTE),
      WT_OPEN_CONFIG_ARG_END};

    printf("Test: basic open with struct config\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_in_memory --
 *     Test in_memory mode.
 */
static void
test_in_memory(void)
{
    WT_CONNECTION *conn;
    WT_OPEN_CONFIG_ARG config[] = {WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
      WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_in_memory, true),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 50 * WT_MEGABYTE),
      WT_OPEN_CONFIG_ARG_END};

    printf("Test: in_memory mode\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_error_prefix --
 *     Test error_prefix config.
 */
static void
test_error_prefix(void)
{
    WT_CONNECTION *conn;
    const char *prefix = "TEST_PREFIX";
    WT_OPEN_CONFIG_ARG config[] = {WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
      WT_OPEN_CONFIG_ARG_SET_STR(WT_OPEN_CONF_error_prefix, prefix, strlen(prefix)),
      WT_OPEN_CONFIG_ARG_END};

    printf("Test: error_prefix config\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_readonly --
 *     Test readonly mode (requires existing database).
 */
static void
test_readonly(void)
{
    WT_CONNECTION *conn;
    WT_OPEN_CONFIG_ARG create_config[] = {
      WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true), WT_OPEN_CONFIG_ARG_END};
    WT_OPEN_CONFIG_ARG readonly_config[] = {
      WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_readonly, true), WT_OPEN_CONFIG_ARG_END};

    printf("Test: readonly mode\n");

    /* First create a database */
    testutil_check(wiredtiger_open_ex(home, NULL, create_config, 0, &conn));
    testutil_check(conn->close(conn, NULL));

    /* Now open it readonly */
    testutil_check(wiredtiger_open_ex(home, NULL, readonly_config, 0, &conn));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_invalid_key --
 *     Test that invalid keys return EINVAL.
 */
static void
test_invalid_key(void)
{
    WT_CONNECTION *conn;
    WT_OPEN_CONFIG_ARG config[] = {WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
      {.key = 999999, .value.v_int = 100, .type = WT_OPEN_CONFIG_ARG_INT}, /* Invalid key */
      WT_OPEN_CONFIG_ARG_END};
    int ret;

    printf("Test: invalid key returns EINVAL\n");

    ret = wiredtiger_open_ex(home, NULL, config, 0, &conn);
    testutil_assert(ret == EINVAL);

    printf("  PASSED\n");
}

/*
 * test_type_mismatch --
 *     Test that passing wrong type for a key returns EINVAL.
 */
static void
test_type_mismatch(void)
{
    WT_CONNECTION *conn;
    int ret;

    printf("Test: type mismatch returns EINVAL\n");

    /*
     * Pass a string type for cache_size which expects int. Manually construct the arg to force the
     * type mismatch.
     */
    WT_OPEN_CONFIG_ARG config[] = {WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
      {.key = WT_OPEN_CONF_cache_size,
        .value.v_str = {.str = "wrong", .len = 5},
        .type = WT_OPEN_CONFIG_ARG_STR},
      WT_OPEN_CONFIG_ARG_END};

    ret = wiredtiger_open_ex(home, NULL, config, 0, &conn);
    testutil_assert(ret == EINVAL);

    printf("  PASSED\n");
}

/*
 * test_duplicate_key --
 *     Test that duplicate keys return EINVAL.
 */
static void
test_duplicate_key(void)
{
    WT_CONNECTION *conn;
    int ret;

    printf("Test: duplicate key returns EINVAL\n");

    /* Pass cache_size twice - should be an error */
    WT_OPEN_CONFIG_ARG config[] = {WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 50 * 1024 * 1024),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 60 * 1024 * 1024), /* Duplicate! */
      WT_OPEN_CONFIG_ARG_END};

    ret = wiredtiger_open_ex(home, NULL, config, 0, &conn);
    testutil_assert(ret == EINVAL);

    printf("  PASSED\n");
}

/*
 * test_empty_config --
 *     Test opening with empty config (just defaults).
 */
static void
test_empty_config(void)
{
    WT_CONNECTION *conn;
    WT_OPEN_CONFIG_ARG config[] = {
      WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true), WT_OPEN_CONFIG_ARG_END};

    printf("Test: empty config (defaults)\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_counted_array --
 *     Test using counted array instead of sentinel.
 */
static void
test_counted_array(void)
{
    WT_CONNECTION *conn;
    WT_OPEN_CONFIG_ARG config[] = {
      WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 100 * WT_MEGABYTE),
    };

    printf("Test: counted array\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 2, &conn));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_eviction_config --
 *     Test eviction configuration (nested keys).
 */
static void
test_eviction_config(void)
{
    WT_CONNECTION *conn;
    WT_OPEN_CONFIG_ARG config[] = {WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 100 * WT_MEGABYTE),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_target, 75),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_trigger, 95),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_dirty_target, 5),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_dirty_trigger, 20),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_threads_min, 2),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_threads_max, 4), WT_OPEN_CONFIG_ARG_END};

    printf("Test: eviction configuration (nested keys)\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_log_config --
 *     Test log configuration (nested keys).
 */
static void
test_log_config(void)
{
    WT_CONNECTION *conn;
    char log_dir[256];
    WT_OPEN_CONFIG_ARG config[] = {WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 100 * WT_MEGABYTE),
      WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_log_enabled, true),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_log_file_max, 10 * WT_MEGABYTE),
      WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_log_prealloc, false), WT_OPEN_CONFIG_ARG_END};

    printf("Test: log configuration (nested keys)\n");

    /* Create the journal directory within the home directory */
    testutil_check(__wt_snprintf(log_dir, sizeof(log_dir), "%s/journal", home));
    testutil_recreate_dir(log_dir);

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_checkpoint_config --
 *     Test checkpoint configuration (nested keys).
 */
static void
test_checkpoint_config(void)
{
    WT_CONNECTION *conn;
    WT_OPEN_CONFIG_ARG config[] = {WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 100 * WT_MEGABYTE),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_checkpoint_wait, 60),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_checkpoint_log_size, 2LL * WT_GIGABYTE),
      WT_OPEN_CONFIG_ARG_END};

    printf("Test: checkpoint configuration (nested keys)\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_multiple_options --
 *     Test multiple configuration options together.
 */
static void
test_multiple_options(void)
{
    WT_CONNECTION *conn;
    WT_OPEN_CONFIG_ARG config[] = {WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_create, true),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_size, 256 * WT_MEGABYTE),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_cache_overhead, 10),
      WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_mmap, false),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_session_max, 100),
      WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_cache_cursors, true),
      WT_OPEN_CONFIG_ARG_SET_BOOL(WT_OPEN_CONF_checkpoint_sync, true),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_threads_min, 1),
      WT_OPEN_CONFIG_ARG_SET_INT(WT_OPEN_CONF_eviction_threads_max, 8), WT_OPEN_CONFIG_ARG_END};

    printf("Test: multiple configuration options\n");

    testutil_check(wiredtiger_open_ex(home, NULL, config, 0, &conn));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * main --
 *     Entry point.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    home = opts->home;

    /* Clean up from previous runs */
    testutil_recreate_dir(home);

    /* Run tests */
    test_basic_open();

    testutil_recreate_dir(home);
    test_in_memory();

    testutil_recreate_dir(home);
    test_error_prefix();

    testutil_recreate_dir(home);
    test_readonly();

    testutil_recreate_dir(home);
    test_invalid_key();

    testutil_recreate_dir(home);
    test_type_mismatch();

    testutil_recreate_dir(home);
    test_duplicate_key();

    testutil_recreate_dir(home);
    test_empty_config();

    testutil_recreate_dir(home);
    test_counted_array();

    testutil_recreate_dir(home);
    test_eviction_config();

    testutil_recreate_dir(home);
    test_log_config();

    testutil_recreate_dir(home);
    test_checkpoint_config();

    testutil_recreate_dir(home);
    test_multiple_options();

    printf("\nAll tests PASSED!\n");

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
