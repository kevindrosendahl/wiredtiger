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

#include <sys/stat.h>

/*
 * Test that log files are truncated to their actual data size at clean shutdown. This speeds up
 * recovery by eliminating the need to scan pre-allocated zeros in __log_has_hole.
 */

static char home[1024];
static const char *const uri = "table:test";

/* Use 1MB log file size for testing. */
#define FILE_MAX (1024 * 1024)
#define LOG_FILE_1 "WiredTigerLog.0000000001"

#define ENV_CONFIG_BASE "create,log=(enabled,file_max=1M,remove=false)"
#define ENV_CONFIG_REOPEN "log=(recover=on)"

/*
 * get_file_size --
 *     Get the size of a file.
 */
static wt_off_t
get_file_size(const char *path)
{
    struct stat sb;

    testutil_assert_errno(stat(path, &sb) == 0);
    return (sb.st_size);
}

/*
 * check_log_truncated --
 *     Verify that the log file is truncated (smaller than file_max).
 */
static void
check_log_truncated(void)
{
    wt_off_t size;
    char path[1024];

    testutil_snprintf(path, sizeof(path), "%s/%s", home, LOG_FILE_1);
    size = get_file_size(path);

    printf("Log file size after shutdown: %" PRId64 " bytes\n", (int64_t)size);

    /*
     * The log file should be truncated to much less than file_max. Allow some overhead for log
     * records, but it should be well under 200KB for our small test data.
     */
    testutil_assert(size < 200 * 1024);
    testutil_assert(size < FILE_MAX);
}

/*
 * insert_data --
 *     Insert test data into the table.
 */
static void
insert_data(WT_SESSION *session, uint32_t start, uint32_t count)
{
    WT_CURSOR *cursor;
    uint32_t i;
    char key[64], value[256];

    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    for (i = start; i < start + count; i++) {
        testutil_snprintf(key, sizeof(key), "key%06" PRIu32, i);
        testutil_snprintf(value, sizeof(value), "value%06" PRIu32, i);
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));
}

/*
 * verify_data --
 *     Verify that all expected data exists.
 */
static void
verify_data(WT_SESSION *session, uint32_t expected_count)
{
    WT_CURSOR *cursor;
    uint32_t count;

    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    count = 0;
    while (cursor->next(cursor) == 0)
        count++;
    testutil_check(cursor->close(cursor));

    printf("Verified %" PRIu32 " records (expected %" PRIu32 ")\n", count, expected_count);
    testutil_assert(count == expected_count);
}

/*
 * test_basic_truncation --
 *     Test that log files are truncated after clean shutdown.
 */
static void
test_basic_truncation(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;

    printf("\n=== Test: Basic log truncation at shutdown ===\n");

    /* Clean start. */
    testutil_recreate_dir(home);

    /* Create database and insert some data. */
    testutil_check(wiredtiger_open(home, NULL, ENV_CONFIG_BASE, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, uri, "key_format=S,value_format=S"));
    insert_data(session, 0, 100);
    testutil_check(session->checkpoint(session, NULL));
    testutil_check(conn->close(conn, NULL));

    /* Verify log file is truncated. */
    check_log_truncated();

    /* Reopen and verify data. */
    testutil_check(wiredtiger_open(home, NULL, ENV_CONFIG_REOPEN, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    verify_data(session, 100);
    testutil_check(conn->close(conn, NULL));

    printf("=== Test passed ===\n");
}

/*
 * test_recovery_with_truncated_log --
 *     Test that recovery works correctly with truncated log files.
 */
static void
test_recovery_with_truncated_log(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;

    printf("\n=== Test: Recovery with truncated log ===\n");

    /* Clean start. */
    testutil_recreate_dir(home);

    /* Create database and insert data, checkpoint. */
    testutil_check(wiredtiger_open(home, NULL, ENV_CONFIG_BASE, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, uri, "key_format=S,value_format=S"));
    insert_data(session, 0, 100);
    testutil_check(session->checkpoint(session, NULL));

    /* Insert more data after checkpoint (needs log recovery). */
    insert_data(session, 100, 100);
    testutil_check(conn->close(conn, NULL));

    /* Verify truncation. */
    check_log_truncated();

    /* Reopen - this triggers log recovery with the truncated file. */
    testutil_check(wiredtiger_open(home, NULL, ENV_CONFIG_REOPEN, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Verify all data including post-checkpoint data. */
    verify_data(session, 200);
    testutil_check(conn->close(conn, NULL));

    printf("=== Test passed ===\n");
}

/*
 * test_multiple_restart_cycles --
 *     Test multiple shutdown/restart cycles with truncation.
 */
static void
test_multiple_restart_cycles(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    uint32_t cycle, total_records;

    printf("\n=== Test: Multiple restart cycles ===\n");

    /* Clean start. */
    testutil_recreate_dir(home);

    total_records = 0;
    for (cycle = 0; cycle < 5; cycle++) {
        printf("Cycle %" PRIu32 "\n", cycle);

        if (cycle == 0)
            testutil_check(wiredtiger_open(home, NULL, ENV_CONFIG_BASE, &conn));
        else
            testutil_check(wiredtiger_open(home, NULL, ENV_CONFIG_REOPEN, &conn));

        testutil_check(conn->open_session(conn, NULL, NULL, &session));

        if (cycle == 0)
            testutil_check(session->create(session, uri, "key_format=S,value_format=S"));

        /* Insert data for this cycle. */
        insert_data(session, total_records, 50);
        total_records += 50;

        /* Checkpoint on even cycles. */
        if (cycle % 2 == 0)
            testutil_check(session->checkpoint(session, NULL));

        /* Close (triggers truncation). */
        testutil_check(conn->close(conn, NULL));

        /* Verify truncation. */
        check_log_truncated();
    }

    /* Final verification of all data. */
    testutil_check(wiredtiger_open(home, NULL, ENV_CONFIG_REOPEN, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    verify_data(session, total_records);
    testutil_check(conn->close(conn, NULL));

    printf("=== Test passed ===\n");
}

/*
 * get_total_log_size --
 *     Get total size of all log files.
 */
static wt_off_t
get_total_log_size(void)
{
    struct stat sb;
    wt_off_t total;
    uint32_t i;
    char path[1024];

    total = 0;
    for (i = 1; i <= 100; i++) {
        testutil_snprintf(path, sizeof(path), "%s/WiredTigerLog.%010" PRIu32, home, i);
        if (stat(path, &sb) == 0)
            total += sb.st_size;
    }
    return (total);
}

/*
 * test_file_grows_after_truncate --
 *     Test that truncated log files can grow on subsequent sessions.
 */
static void
test_file_grows_after_truncate(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    wt_off_t size_first, size_second;

    printf("\n=== Test: File grows after truncation ===\n");

    /* Clean start. */
    testutil_recreate_dir(home);

    /* Create database with small amount of data. */
    testutil_check(wiredtiger_open(home, NULL, ENV_CONFIG_BASE, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, uri, "key_format=S,value_format=S"));
    insert_data(session, 0, 10);
    testutil_check(conn->close(conn, NULL));

    size_first = get_total_log_size();
    printf("Total log size after first session: %" PRId64 " bytes\n", (int64_t)size_first);
    testutil_assert(size_first > 0);

    /* Reopen and insert more data (moderate amount to avoid rotation complexity). */
    testutil_check(wiredtiger_open(home, NULL, ENV_CONFIG_REOPEN, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    insert_data(session, 10, 100);
    testutil_check(conn->close(conn, NULL));

    size_second = get_total_log_size();
    printf("Total log size after second session: %" PRId64 " bytes\n", (int64_t)size_second);

    /* Total log size should have grown. */
    testutil_assert(size_second > size_first);

    /* Verify all data. */
    testutil_check(wiredtiger_open(home, NULL, ENV_CONFIG_REOPEN, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    verify_data(session, 110);
    testutil_check(conn->close(conn, NULL));

    printf("=== Test passed ===\n");
}

/*
 * main --
 *     Main entry point for log truncation at shutdown test.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    testutil_work_dir_from_path(home, sizeof(home), opts->home);

    test_basic_truncation();
    test_recovery_with_truncated_log();
    test_multiple_restart_cycles();
    test_file_grows_after_truncate();

    printf("\n*** All log truncation tests passed ***\n");

    /* Final cleanup is done implicitly by the test framework. */
    if (!opts->preserve)
        testutil_remove(home);

    return (EXIT_SUCCESS);
}
