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

/*
 * Test program for btree configuration cache.
 *
 * This verifies that btree config is cached properly and that subsequent table opens use the cached
 * values rather than re-parsing the metadata.
 */

static const char *home;

/*
 * get_stat --
 *     Get a data source statistic value.
 */
static int64_t
get_stat(WT_SESSION *session, const char *uri, const char *stat_name)
{
    WT_CURSOR *cursor;
    int64_t val;
    const char *desc, *pvalue;
    char stat_uri[256];

    testutil_snprintf(stat_uri, sizeof(stat_uri), "statistics:%s", uri);
    testutil_check(session->open_cursor(session, stat_uri, NULL, "statistics=(all)", &cursor));

    /* Search for the statistic */
    while (cursor->next(cursor) == 0) {
        testutil_check(cursor->get_value(cursor, &desc, &pvalue, &val));
        if (strstr(desc, stat_name) != NULL) {
            testutil_check(cursor->close(cursor));
            return val;
        }
    }
    testutil_check(cursor->close(cursor));
    return -1;
}

/*
 * test_cache_hit --
 *     Test that the cache is populated and values are correct. Note: The btree is only configured
 *     once per dhandle, so the cache hit/miss stats measure dhandle-level caching, not per-cursor.
 */
static void
test_cache_hit(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_CURSOR *cursor;
    int64_t misses;
    int i;

    printf("Test: btree config cache population\n");

    testutil_check(wiredtiger_open(home, NULL, "create,statistics=(all)", &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create table */
    testutil_check(session->create(session, "table:test",
      "key_format=i,value_format=S,allocation_size=4096,leaf_page_max=16384"));

    /* First open - cache miss expected */
    testutil_check(session->open_cursor(session, "table:test", NULL, NULL, &cursor));
    testutil_check(cursor->close(cursor));

    /* Get stats after first open */
    misses = get_stat(session, "table:test", "btree config cache miss");
    printf("  After first open - misses: %" PRId64 "\n", misses);

    /* First open should populate the cache (1 miss) */
    testutil_assert(misses >= 1);

    /* Insert some data and verify it works */
    testutil_check(session->open_cursor(session, "table:test", NULL, NULL, &cursor));
    for (i = 0; i < 100; i++) {
        cursor->set_key(cursor, i);
        cursor->set_value(cursor, "test_value");
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));

    /* Verify data can be read back */
    testutil_check(session->open_cursor(session, "table:test", NULL, NULL, &cursor));
    cursor->set_key(cursor, 50);
    testutil_check(cursor->search(cursor));
    testutil_check(cursor->close(cursor));

    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_config_values --
 *     Test that cached values are correct.
 */
static void
test_config_values(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_CURSOR *cursor;
    int64_t alloc_size, leaf_max;

    printf("Test: cached config values correctness\n");

    testutil_check(wiredtiger_open(home, NULL, "create,statistics=(all)", &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create table with specific config values - use defaults that are known to be valid */
    testutil_check(session->create(session, "table:test",
      "key_format=q,value_format=u,allocation_size=4096,leaf_page_max=32768"));

    /* Open cursor (populates cache) */
    testutil_check(session->open_cursor(session, "table:test", NULL, NULL, &cursor));
    testutil_check(cursor->close(cursor));

    /* Check the statistics to verify allocation size */
    alloc_size = get_stat(session, "table:test", "file allocation unit size");
    leaf_max = get_stat(session, "table:test", "maximum leaf page size");

    printf("  allocation_size: %" PRId64 ", leaf_page_max: %" PRId64 "\n", alloc_size, leaf_max);

    /* Verify the config values are correct */
    testutil_assert(alloc_size == 4096);
    testutil_assert(leaf_max == 32768);

    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_multiple_tables --
 *     Test that each table has its own cache.
 */
static void
test_multiple_tables(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_CURSOR *cursor1, *cursor2;
    int64_t leaf1, leaf2;

    printf("Test: multiple tables have separate caches\n");

    testutil_check(wiredtiger_open(home, NULL, "create,statistics=(all)", &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create two tables with different leaf_page_max configs */
    testutil_check(
      session->create(session, "table:test1", "key_format=i,value_format=S,leaf_page_max=16384"));
    testutil_check(
      session->create(session, "table:test2", "key_format=i,value_format=S,leaf_page_max=65536"));

    /* Open cursors on both tables */
    testutil_check(session->open_cursor(session, "table:test1", NULL, NULL, &cursor1));
    testutil_check(session->open_cursor(session, "table:test2", NULL, NULL, &cursor2));

    /* Check leaf page sizes */
    leaf1 = get_stat(session, "table:test1", "maximum leaf page size");
    leaf2 = get_stat(session, "table:test2", "maximum leaf page size");

    printf("  table:test1 leaf_page_max: %" PRId64 "\n", leaf1);
    printf("  table:test2 leaf_page_max: %" PRId64 "\n", leaf2);

    testutil_assert(leaf1 == 16384);
    testutil_assert(leaf2 == 65536);

    testutil_check(cursor1->close(cursor1));
    testutil_check(cursor2->close(cursor2));
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_row_store_config --
 *     Test row-store specific config (prefix compression).
 */
static void
test_row_store_config(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_CURSOR *cursor;
    int i;
    int key;
    const char *value;

    printf("Test: row-store specific config (prefix compression)\n");

    testutil_check(wiredtiger_open(home, NULL, "create,statistics=(all)", &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create row-store table with prefix compression */
    testutil_check(session->create(session, "table:test",
      "key_format=i,value_format=S,prefix_compression=true,prefix_compression_min=4"));

    /* Insert some data to exercise the config */
    testutil_check(session->open_cursor(session, "table:test", NULL, NULL, &cursor));
    for (i = 0; i < 100; i++) {
        cursor->set_key(cursor, i);
        cursor->set_value(cursor, "test_value");
        testutil_check(cursor->insert(cursor));
    }

    /* Read back data */
    cursor->reset(cursor);
    i = 0;
    while (cursor->next(cursor) == 0) {
        testutil_check(cursor->get_key(cursor, &key));
        testutil_check(cursor->get_value(cursor, &value));
        testutil_assert(key == i);
        testutil_assert(strcmp(value, "test_value") == 0);
        i++;
    }
    testutil_assert(i == 100);

    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_column_store_config --
 *     Test column-store specific config.
 */
static void
test_column_store_config(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_CURSOR *cursor;
    uint64_t key;
    const char *value;
    int i;

    printf("Test: column-store specific config\n");

    testutil_check(wiredtiger_open(home, NULL, "create,statistics=(all)", &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create column-store table */
    testutil_check(
      session->create(session, "table:test", "key_format=r,value_format=S,dictionary=100"));

    /* Insert some data */
    testutil_check(session->open_cursor(session, "table:test", NULL, "append", &cursor));
    for (i = 0; i < 100; i++) {
        cursor->set_value(cursor, "test_value");
        testutil_check(cursor->insert(cursor));
    }

    /* Read back data */
    cursor->reset(cursor);
    i = 0;
    while (cursor->next(cursor) == 0) {
        testutil_check(cursor->get_key(cursor, &key));
        testutil_check(cursor->get_value(cursor, &value));
        testutil_assert(key == (uint64_t)(i + 1));
        testutil_assert(strcmp(value, "test_value") == 0);
        i++;
    }
    testutil_assert(i == 100);

    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_cache_invalidation_on_alter --
 *     Test that the cache is invalidated after session->alter().
 */
static void
test_cache_invalidation_on_alter(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_CURSOR *cursor;
    int64_t misses_before, misses_after;

    printf("Test: cache invalidation after alter\n");

    testutil_check(wiredtiger_open(home, NULL, "create,statistics=(all)", &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create table and open cursor to populate cache */
    testutil_check(
      session->create(session, "table:test", "key_format=i,value_format=S,cache_resident=false"));
    testutil_check(session->open_cursor(session, "table:test", NULL, NULL, &cursor));
    testutil_check(cursor->close(cursor));

    /* Get cache miss count before alter */
    misses_before = get_stat(session, "table:test", "btree config cache miss");

    /* Alter the table - this should invalidate the cache */
    testutil_check(session->alter(session, "table:test", "cache_resident=true"));

    /* Open cursor again - should be a cache miss due to invalidation */
    testutil_check(session->open_cursor(session, "table:test", NULL, NULL, &cursor));
    testutil_check(cursor->close(cursor));

    /* Get cache miss count after alter */
    misses_after = get_stat(session, "table:test", "btree config cache miss");

    printf("  Cache misses before alter: %" PRId64 ", after: %" PRId64 "\n", misses_before,
      misses_after);

    /* After alter, there should be at least one more cache miss */
    testutil_assert(misses_after > misses_before);

    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * main --
 *     Run btree config cache tests.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    home = opts->home;

    testutil_recreate_dir(home);
    test_cache_hit();

    testutil_recreate_dir(home);
    test_config_values();

    testutil_recreate_dir(home);
    test_multiple_tables();

    testutil_recreate_dir(home);
    test_row_store_config();

    testutil_recreate_dir(home);
    test_column_store_config();

    testutil_recreate_dir(home);
    test_cache_invalidation_on_alter();

    printf("\nAll btree config cache tests PASSED\n");

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
