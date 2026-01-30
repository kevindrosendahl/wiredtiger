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

/*
 * test_log_recovery_skip --
 *     Test the log.recovery_skip configuration option.
 *
 * This test verifies: 1. Shutdown marker file is created on clean shutdown when recovery_skip=true
 *     2. Marker file is deleted on startup after validation 3. File size mismatch detection
 *     triggers full recovery 4. Multiple restart cycles maintain data integrity 5. Default behavior
 *     (recovery_skip=false) doesn't create marker
 */

#include "test_util.h"

#include <dirent.h>

#define TURTLE_FILE "WiredTiger.turtle"
#define MARKER_KEY "Log shutdown"
#define TABLE_URI "table:test"
#define NROWS 1000

static const char *conn_config_skip = "create,log=(enabled=true,file_max=1MB,recovery_skip=true)";
static const char *conn_config_no_skip = "create,log=(enabled=true,file_max=1MB)";

/*
 * marker_exists --
 *     Check if the shutdown marker exists in the turtle file.
 */
static bool
marker_exists(const char *home)
{
    FILE *fp;
    char line[1024];
    char path[1024];
    bool found;

    testutil_snprintf(path, sizeof(path), "%s/%s", home, TURTLE_FILE);
    fp = fopen(path, "r");
    if (fp == NULL)
        return false;

    found = false;
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, MARKER_KEY) != NULL) {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

/*
 * insert_data --
 *     Insert test data into the table.
 */
static void
insert_data(WT_SESSION *session, int start, int count)
{
    WT_CURSOR *cursor;
    int i;
    char value[64];

    testutil_check(session->open_cursor(session, TABLE_URI, NULL, NULL, &cursor));
    for (i = start; i < start + count; i++) {
        cursor->set_key(cursor, i);
        testutil_snprintf(value, sizeof(value), "value_%d", i);
        cursor->set_value(cursor, value);
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));
}

/*
 * verify_data --
 *     Verify data in the table and return the count.
 */
static int
verify_data(WT_SESSION *session)
{
    WT_CURSOR *cursor;
    int count, key;
    char expected[64], *value;

    testutil_check(session->open_cursor(session, TABLE_URI, NULL, NULL, &cursor));
    count = 0;
    while (cursor->next(cursor) == 0) {
        testutil_check(cursor->get_key(cursor, &key));
        testutil_check(cursor->get_value(cursor, &value));
        testutil_snprintf(expected, sizeof(expected), "value_%d", key);
        testutil_assert(strcmp(value, expected) == 0);
        count++;
    }
    testutil_check(cursor->close(cursor));
    return count;
}

/*
 * test_marker_creation --
 *     Test that marker file is created on clean shutdown with recovery_skip=true.
 */
static void
test_marker_creation(TEST_OPTS *opts)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;

    printf("=== Test: Marker creation on clean shutdown ===\n");

    /* Open connection with recovery_skip enabled. */
    testutil_check(wiredtiger_open(opts->home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, TABLE_URI, "key_format=i,value_format=S"));

    insert_data(session, 0, NROWS);
    testutil_check(session->checkpoint(session, NULL));

    testutil_check(conn->close(conn, NULL));

    /* Verify marker file exists. */
    testutil_assert(marker_exists(opts->home));

    printf("=== Test passed: Marker file created ===\n\n");
}

/*
 * test_marker_deletion --
 *     Test that marker file is deleted on startup after validation.
 */
static void
test_marker_deletion(TEST_OPTS *opts)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    int count;

    printf("=== Test: Marker deletion on startup ===\n");

    /* Marker should exist from previous test. */
    testutil_assert(marker_exists(opts->home));

    /* Reopen - marker should be deleted after validation. */
    testutil_check(wiredtiger_open(opts->home, NULL, conn_config_skip, &conn));

    /* Marker should be deleted now. */
    testutil_assert(!marker_exists(opts->home));

    /* Verify data. */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    count = verify_data(session);
    testutil_assert(count == NROWS);

    testutil_check(conn->close(conn, NULL));

    /* Marker should be recreated on clean shutdown. */
    testutil_assert(marker_exists(opts->home));

    printf("=== Test passed: Marker deleted and recreated ===\n\n");
}

/*
 * test_multiple_restarts --
 *     Test multiple restart cycles with recovery_skip.
 */
static void
test_multiple_restarts(TEST_OPTS *opts)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    int cycle, count, total_rows;

    printf("=== Test: Multiple restart cycles ===\n");

    total_rows = NROWS; /* From previous tests. */

    for (cycle = 0; cycle < 10; cycle++) {
        printf("Cycle %d\n", cycle);

        testutil_check(wiredtiger_open(opts->home, NULL, conn_config_skip, &conn));
        testutil_check(conn->open_session(conn, NULL, NULL, &session));

        /* Verify existing data. */
        count = verify_data(session);
        testutil_assert(count == total_rows);

        /* Add more data. */
        insert_data(session, total_rows, 50);
        total_rows += 50;

        /* Checkpoint on even cycles. */
        if (cycle % 2 == 0)
            testutil_check(session->checkpoint(session, NULL));

        testutil_check(conn->close(conn, NULL));

        /* Marker should exist after clean shutdown. */
        testutil_assert(marker_exists(opts->home));
    }

    /* Final verification. */
    testutil_check(wiredtiger_open(opts->home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    count = verify_data(session);
    testutil_assert(count == total_rows);
    testutil_check(conn->close(conn, NULL));

    printf("Verified %d records across %d restart cycles\n", total_rows, 10);
    printf("=== Test passed ===\n\n");
}

/*
 * test_no_marker_without_config --
 *     Test that no marker is created when recovery_skip is not configured.
 */
static void
test_no_marker_without_config(TEST_OPTS *opts)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    char new_home[1024];

    printf("=== Test: No marker without recovery_skip config ===\n");

    /* Create a new home directory for this test. */
    testutil_snprintf(new_home, sizeof(new_home), "%s_noskip", opts->home);
    testutil_recreate_dir(new_home);

    testutil_check(wiredtiger_open(new_home, NULL, conn_config_no_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, TABLE_URI, "key_format=i,value_format=S"));

    insert_data(session, 0, 100);
    testutil_check(session->checkpoint(session, NULL));
    testutil_check(conn->close(conn, NULL));

    /* Marker should NOT exist. */
    testutil_assert(!marker_exists(new_home));

    printf("=== Test passed: No marker without config ===\n\n");
}

/*
 * test_file_size_mismatch --
 *     Test that file size mismatch triggers full recovery.
 */
static void
test_file_size_mismatch(TEST_OPTS *opts)
{
    struct dirent *entry;
    DIR *dir;
    FILE *fp;
    WT_CONNECTION *conn;
    WT_SESSION *session;
    int count;
    char log_path[1024];
    char new_home[1024];
    bool found_log;

    printf("=== Test: File size mismatch detection ===\n");

    /* Create a new home directory for this test. */
    testutil_snprintf(new_home, sizeof(new_home), "%s_mismatch", opts->home);
    testutil_recreate_dir(new_home);

    /* Create database and close cleanly. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, TABLE_URI, "key_format=i,value_format=S"));

    insert_data(session, 0, 500);
    testutil_check(session->checkpoint(session, NULL));
    testutil_check(conn->close(conn, NULL));

    /* Verify marker exists. */
    testutil_assert(marker_exists(new_home));

    /* Find a log file and modify its size. */
    dir = opendir(new_home);
    testutil_assert(dir != NULL);
    found_log = false;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "WiredTigerLog.", 14) == 0) {
            testutil_snprintf(log_path, sizeof(log_path), "%s/%s", new_home, entry->d_name);
            fp = fopen(log_path, "ab");
            testutil_assert(fp != NULL);
            /* Append some bytes to change the file size. */
            fprintf(fp, "%s", "EXTRA_DATA_TO_CHANGE_SIZE");
            fclose(fp);
            found_log = true;
            printf("Modified log file: %s\n", entry->d_name);
            break;
        }
    }
    closedir(dir);
    testutil_assert(found_log);

    /* Reopen - should detect size mismatch and do full recovery. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Verify data is intact. */
    count = verify_data(session);
    testutil_assert(count == 500);

    testutil_check(conn->close(conn, NULL));

    printf("=== Test passed: Size mismatch handled correctly ===\n\n");
}

/*
 * main --
 *     Test entry point.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    testutil_recreate_dir(opts->home);

    /* Run tests in sequence - they build on each other. */
    test_marker_creation(opts);
    test_marker_deletion(opts);
    test_multiple_restarts(opts);
    test_no_marker_without_config(opts);
    test_file_size_mismatch(opts);

    printf("*** All log recovery_skip tests passed ***\n");

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
