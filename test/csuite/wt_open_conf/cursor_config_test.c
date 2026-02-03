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
 * Test program for cursor config fast path statistics.
 *
 * This verifies that the NULL/empty config and "overwrite=false" fast paths are properly tracked
 * via statistics.
 */

static const char *home;

/*
 * get_stat --
 *     Get a connection statistic value.
 */
static int64_t
get_stat(WT_SESSION *session, const char *stat_name)
{
    WT_CURSOR *cursor;
    int64_t val;
    const char *desc, *pvalue;
    char stat_uri[256];

    testutil_snprintf(stat_uri, sizeof(stat_uri), "statistics:");
    testutil_check(session->open_cursor(session, stat_uri, NULL, NULL, &cursor));

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
 * test_null_config_fast_path --
 *     Test that NULL config triggers the fast path.
 */
static void
test_null_config_fast_path(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_CURSOR *cursor;
    int64_t fast_null_before, fast_null_after;
    int i;

    printf("Test: NULL config fast path\n");

    testutil_check(wiredtiger_open(home, NULL, "create,statistics=(all)", &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create a table */
    testutil_check(session->create(session, "table:test", "key_format=i,value_format=S"));

    /* Get initial stat value */
    fast_null_before = get_stat(session, "cursor open with NULL/empty config");

    /* Open cursors with NULL config multiple times */
    for (i = 0; i < 100; i++) {
        testutil_check(session->open_cursor(session, "table:test", NULL, NULL, &cursor));
        testutil_check(cursor->close(cursor));
    }

    /* Check that fast path stat increased */
    fast_null_after = get_stat(session, "cursor open with NULL/empty config");

    printf("  Fast path NULL before: %" PRId64 ", after: %" PRId64 "\n", fast_null_before,
      fast_null_after);

    /*
     * The difference may not be exactly 100 because the cursor cache may be involved, but we should
     * see a significant increase.
     */
    testutil_assert(fast_null_after > fast_null_before);

    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_overwrite_false_fast_path --
 *     Test that "overwrite=false" config triggers the fast path.
 */
static void
test_overwrite_false_fast_path(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_CURSOR *cursor;
    int64_t fast_overwrite_before, fast_overwrite_after;
    int i;

    printf("Test: overwrite=false fast path\n");

    testutil_check(wiredtiger_open(home, NULL, "create,statistics=(all)", &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create a table */
    testutil_check(session->create(session, "table:test", "key_format=i,value_format=S"));

    /* Get initial stat value */
    fast_overwrite_before = get_stat(session, "cursor open with overwrite=false");

    /* Open cursors with overwrite=false multiple times */
    for (i = 0; i < 100; i++) {
        testutil_check(
          session->open_cursor(session, "table:test", NULL, "overwrite=false", &cursor));
        testutil_check(cursor->close(cursor));
    }

    /* Check that fast path stat increased */
    fast_overwrite_after = get_stat(session, "cursor open with overwrite=false");

    printf("  Fast path overwrite=false before: %" PRId64 ", after: %" PRId64 "\n",
      fast_overwrite_before, fast_overwrite_after);

    testutil_assert(fast_overwrite_after > fast_overwrite_before);

    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_slow_path --
 *     Test that complex config triggers the slow path.
 */
static void
test_slow_path(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_CURSOR *cursor;
    int64_t slow_before, slow_after;
    int i;

    printf("Test: slow path for complex config\n");

    testutil_check(wiredtiger_open(home, NULL, "create,statistics=(all)", &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create a table */
    testutil_check(session->create(session, "table:test", "key_format=i,value_format=S"));

    /* Get initial stat value */
    slow_before = get_stat(session, "cursor open requiring full config parse");

    /* Open cursors with complex config (forces slow path) */
    for (i = 0; i < 50; i++) {
        testutil_check(
          session->open_cursor(session, "table:test", NULL, "overwrite=false,raw=true", &cursor));
        testutil_check(cursor->close(cursor));
    }

    /* Check that slow path stat increased */
    slow_after = get_stat(session, "cursor open requiring full config parse");

    printf("  Slow path before: %" PRId64 ", after: %" PRId64 "\n", slow_before, slow_after);

    testutil_assert(slow_after > slow_before);

    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * test_empty_string_config --
 *     Test that empty string config triggers the NULL fast path.
 */
static void
test_empty_string_config(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_CURSOR *cursor;
    int64_t fast_null_before, fast_null_after;
    int i;

    printf("Test: empty string config fast path\n");

    testutil_check(wiredtiger_open(home, NULL, "create,statistics=(all)", &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create a table */
    testutil_check(session->create(session, "table:test", "key_format=i,value_format=S"));

    /* Get initial stat value */
    fast_null_before = get_stat(session, "cursor open with NULL/empty config");

    /* Open cursors with empty string config */
    for (i = 0; i < 100; i++) {
        testutil_check(session->open_cursor(session, "table:test", NULL, "", &cursor));
        testutil_check(cursor->close(cursor));
    }

    /* Check that fast path stat increased */
    fast_null_after = get_stat(session, "cursor open with NULL/empty config");

    printf("  Fast path empty string before: %" PRId64 ", after: %" PRId64 "\n", fast_null_before,
      fast_null_after);

    testutil_assert(fast_null_after > fast_null_before);

    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    printf("  PASSED\n");
}

/*
 * main --
 *     Run cursor config fast path tests.
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

    test_null_config_fast_path();
    testutil_recreate_dir(home);

    test_overwrite_false_fast_path();
    testutil_recreate_dir(home);

    test_slow_path();
    testutil_recreate_dir(home);

    test_empty_string_config();

    printf("\nAll cursor config fast path tests PASSED\n");

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
