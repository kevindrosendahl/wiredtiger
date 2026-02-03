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
 *     (recovery_skip=false) doesn't create marker 6. Extended marker format includes max_fileid and
 *     hs_exists 7. Corruption of any marker field triggers full recovery 8. Multiple tables result
 *     in correct max_fileid
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
 * get_marker_value --
 *     Get the shutdown marker value from the turtle file. Returns allocated string or NULL.
 */
static char *
get_marker_value(const char *home)
{
    FILE *fp;
    char line[1024];
    char path[1024];
    char *result;
    bool found_key;

    testutil_snprintf(path, sizeof(path), "%s/%s", home, TURTLE_FILE);
    fp = fopen(path, "r");
    if (fp == NULL)
        return NULL;

    result = NULL;
    found_key = false;
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (found_key) {
            /* Remove trailing newline. */
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n')
                line[len - 1] = '\0';
            result = strdup(line);
            break;
        }
        if (strstr(line, MARKER_KEY) != NULL)
            found_key = true;
    }
    fclose(fp);
    return result;
}

/*
 * marker_has_extended_fields --
 *     Check if the marker has the extended format with max_fileid and hs_exists.
 */
static bool
marker_has_extended_fields(const char *home)
{
    char *marker;
    bool has_extended;

    marker = get_marker_value(home);
    if (marker == NULL)
        return false;

    has_extended = (strstr(marker, "max_fileid=") != NULL && strstr(marker, "hs_exists=") != NULL);
    free(marker);
    return has_extended;
}

/*
 * corrupt_marker_field --
 *     Corrupt a specific field in the marker by modifying its value.
 */
static void
corrupt_marker_field(const char *home, const char *field_name)
{
    FILE *fp_in, *fp_out;
    char line[1024];
    char path[1024], tmp_path[1024];
    bool found_key, modified;

    testutil_snprintf(path, sizeof(path), "%s/%s", home, TURTLE_FILE);
    testutil_snprintf(tmp_path, sizeof(tmp_path), "%s/%s.tmp", home, TURTLE_FILE);

    fp_in = fopen(path, "r");
    testutil_assert(fp_in != NULL);
    fp_out = fopen(tmp_path, "w");
    testutil_assert(fp_out != NULL);

    found_key = false;
    modified = false;
    while (fgets(line, sizeof(line), fp_in) != NULL) {
        if (found_key && !modified) {
            /* This is the marker value line - corrupt the specified field. */
            char *field_pos = strstr(line, field_name);
            if (field_pos != NULL) {
                char *eq_pos = strchr(field_pos, '=');
                if (eq_pos != NULL) {
                    /* Change the first digit after '=' to corrupt the value. */
                    eq_pos++;
                    while (*eq_pos == ' ')
                        eq_pos++;
                    if (*eq_pos >= '0' && *eq_pos <= '8')
                        *eq_pos = *eq_pos + 1;
                    else if (*eq_pos == '9')
                        *eq_pos = '0';
                }
            }
            modified = true;
        }
        fputs(line, fp_out);
        if (strstr(line, MARKER_KEY) != NULL)
            found_key = true;
    }

    fclose(fp_in);
    fclose(fp_out);

    /* Replace original with modified file. */
    testutil_assert(rename(tmp_path, path) == 0);
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
 * test_extended_marker_format --
 *     Test that the marker includes max_fileid and hs_exists fields.
 */
static void
test_extended_marker_format(TEST_OPTS *opts)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    int64_t file_size;
    uint32_t file_num, offset, max_fileid, checksum;
    int hs_exists, parsed;
    char *marker;
    char new_home[1024];

    printf("=== Test: Extended marker format ===\n");

    /* Create a new home directory for this test. */
    testutil_snprintf(new_home, sizeof(new_home), "%s_extended", opts->home);
    testutil_recreate_dir(new_home);

    /* Create database with recovery_skip. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, TABLE_URI, "key_format=i,value_format=S"));

    insert_data(session, 0, 100);
    testutil_check(session->checkpoint(session, NULL));
    testutil_check(conn->close(conn, NULL));

    /* Verify marker has extended fields. */
    testutil_assert(marker_has_extended_fields(new_home));

    marker = get_marker_value(new_home);
    testutil_assert(marker != NULL);
    printf("Marker value: %s\n", marker);

    /* Parse and verify all fields are present. */
    parsed = sscanf(marker,
      "file=%" SCNu32 ",offset=%" SCNu32 ",file_size=%" SCNd64 ",max_fileid=%" SCNu32
      ",hs_exists=%d,checksum=%" SCNu32,
      &file_num, &offset, &file_size, &max_fileid, &hs_exists, &checksum);
    testutil_assert(parsed == 6);

    /* max_fileid should be > 0 since we created a table. */
    testutil_assert(max_fileid > 0);
    printf("Parsed: file=%u, offset=%u, file_size=%" PRId64 ", max_fileid=%u, hs_exists=%d\n",
      file_num, offset, file_size, max_fileid, hs_exists);

    free(marker);

    printf("=== Test passed: Extended marker format verified ===\n\n");
}

/*
 * test_max_fileid_corruption --
 *     Test that corrupting max_fileid triggers full recovery.
 */
static void
test_max_fileid_corruption(TEST_OPTS *opts)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    int count;
    char new_home[1024];

    printf("=== Test: max_fileid corruption detection ===\n");

    /* Create a new home directory for this test. */
    testutil_snprintf(new_home, sizeof(new_home), "%s_maxfileid", opts->home);
    testutil_recreate_dir(new_home);

    /* Create database with recovery_skip. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, TABLE_URI, "key_format=i,value_format=S"));

    insert_data(session, 0, 200);
    testutil_check(session->checkpoint(session, NULL));
    testutil_check(conn->close(conn, NULL));

    testutil_assert(marker_exists(new_home));

    /* Corrupt the max_fileid field. */
    corrupt_marker_field(new_home, "max_fileid");
    printf("Corrupted max_fileid field in marker\n");

    /* Reopen - should detect corruption via checksum and do full recovery. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Verify data is intact (full recovery should have run). */
    count = verify_data(session);
    testutil_assert(count == 200);

    testutil_check(conn->close(conn, NULL));

    printf("=== Test passed: max_fileid corruption handled ===\n\n");
}

/*
 * test_hs_exists_corruption --
 *     Test that corrupting hs_exists triggers full recovery.
 */
static void
test_hs_exists_corruption(TEST_OPTS *opts)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    int count;
    char new_home[1024];

    printf("=== Test: hs_exists corruption detection ===\n");

    /* Create a new home directory for this test. */
    testutil_snprintf(new_home, sizeof(new_home), "%s_hsexists", opts->home);
    testutil_recreate_dir(new_home);

    /* Create database with recovery_skip. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, TABLE_URI, "key_format=i,value_format=S"));

    insert_data(session, 0, 200);
    testutil_check(session->checkpoint(session, NULL));
    testutil_check(conn->close(conn, NULL));

    testutil_assert(marker_exists(new_home));

    /* Corrupt the hs_exists field. */
    corrupt_marker_field(new_home, "hs_exists");
    printf("Corrupted hs_exists field in marker\n");

    /* Reopen - should detect corruption via checksum and do full recovery. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Verify data is intact (full recovery should have run). */
    count = verify_data(session);
    testutil_assert(count == 200);

    testutil_check(conn->close(conn, NULL));

    printf("=== Test passed: hs_exists corruption handled ===\n\n");
}

/*
 * test_checksum_corruption --
 *     Test that corrupting checksum triggers full recovery.
 */
static void
test_checksum_corruption(TEST_OPTS *opts)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    int count;
    char new_home[1024];

    printf("=== Test: checksum corruption detection ===\n");

    /* Create a new home directory for this test. */
    testutil_snprintf(new_home, sizeof(new_home), "%s_checksum", opts->home);
    testutil_recreate_dir(new_home);

    /* Create database with recovery_skip. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, TABLE_URI, "key_format=i,value_format=S"));

    insert_data(session, 0, 200);
    testutil_check(session->checkpoint(session, NULL));
    testutil_check(conn->close(conn, NULL));

    testutil_assert(marker_exists(new_home));

    /* Corrupt the checksum field directly. */
    corrupt_marker_field(new_home, "checksum");
    printf("Corrupted checksum field in marker\n");

    /* Reopen - should detect checksum mismatch and do full recovery. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Verify data is intact (full recovery should have run). */
    count = verify_data(session);
    testutil_assert(count == 200);

    testutil_check(conn->close(conn, NULL));

    printf("=== Test passed: checksum corruption handled ===\n\n");
}

/*
 * test_multiple_tables_fileid --
 *     Test that max_fileid is correct with multiple tables.
 */
static void
test_multiple_tables_fileid(TEST_OPTS *opts)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    int64_t file_size;
    uint32_t file_num, offset, max_fileid, checksum;
    int hs_exists, i, parsed;
    char *marker;
    char new_home[1024];
    char table_name[64];

    printf("=== Test: Multiple tables max_fileid ===\n");

    /* Create a new home directory for this test. */
    testutil_snprintf(new_home, sizeof(new_home), "%s_multitable", opts->home);
    testutil_recreate_dir(new_home);

    /* Create database with recovery_skip and multiple tables. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create 10 tables. */
    for (i = 0; i < 10; i++) {
        testutil_snprintf(table_name, sizeof(table_name), "table:test%d", i);
        testutil_check(session->create(session, table_name, "key_format=i,value_format=S"));

        testutil_check(session->open_cursor(session, table_name, NULL, NULL, &cursor));
        cursor->set_key(cursor, 1);
        cursor->set_value(cursor, "value");
        testutil_check(cursor->insert(cursor));
        testutil_check(cursor->close(cursor));
    }

    testutil_check(session->checkpoint(session, NULL));
    testutil_check(conn->close(conn, NULL));

    /* Verify marker has extended fields. */
    testutil_assert(marker_has_extended_fields(new_home));

    marker = get_marker_value(new_home);
    testutil_assert(marker != NULL);

    parsed = sscanf(marker,
      "file=%" SCNu32 ",offset=%" SCNu32 ",file_size=%" SCNd64 ",max_fileid=%" SCNu32
      ",hs_exists=%d,checksum=%" SCNu32,
      &file_num, &offset, &file_size, &max_fileid, &hs_exists, &checksum);
    testutil_assert(parsed == 6);

    /*
     * max_fileid should be large enough for the tables we created. The exact number depends on
     * internal allocations (metadata, history store, etc.), but should be at least as many as we
     * created.
     */
    printf("max_fileid after 10 tables: %u\n", max_fileid);
    testutil_assert(max_fileid >= 10);

    free(marker);

    /* Reopen and verify it works. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Verify we can access all tables. */
    for (i = 0; i < 10; i++) {
        testutil_snprintf(table_name, sizeof(table_name), "table:test%d", i);
        testutil_check(session->open_cursor(session, table_name, NULL, NULL, &cursor));
        testutil_check(cursor->next(cursor));
        testutil_check(cursor->close(cursor));
    }

    testutil_check(conn->close(conn, NULL));

    printf("=== Test passed: Multiple tables max_fileid correct ===\n\n");
}

/*
 * test_create_table_after_skip --
 *     Test that creating a new table after recovery_skip works correctly.
 */
static void
test_create_table_after_skip(TEST_OPTS *opts)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    char new_home[1024];

    printf("=== Test: Create table after recovery skip ===\n");

    /* Create a new home directory for this test. */
    testutil_snprintf(new_home, sizeof(new_home), "%s_createafter", opts->home);
    testutil_recreate_dir(new_home);

    /* Create initial database with one table. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, "table:initial", "key_format=i,value_format=S"));

    testutil_check(session->open_cursor(session, "table:initial", NULL, NULL, &cursor));
    cursor->set_key(cursor, 1);
    cursor->set_value(cursor, "initial_value");
    testutil_check(cursor->insert(cursor));
    testutil_check(cursor->close(cursor));

    testutil_check(session->checkpoint(session, NULL));
    testutil_check(conn->close(conn, NULL));

    /* Verify marker exists. */
    testutil_assert(marker_has_extended_fields(new_home));

    /* Reopen with recovery_skip (should skip metadata scan). */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create a new table - this should work correctly with the cached max_fileid. */
    testutil_check(session->create(session, "table:new_table", "key_format=i,value_format=S"));

    testutil_check(session->open_cursor(session, "table:new_table", NULL, NULL, &cursor));
    cursor->set_key(cursor, 1);
    cursor->set_value(cursor, "new_value");
    testutil_check(cursor->insert(cursor));
    testutil_check(cursor->close(cursor));

    testutil_check(session->checkpoint(session, NULL));
    testutil_check(conn->close(conn, NULL));

    /* Reopen and verify both tables exist. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    testutil_check(session->open_cursor(session, "table:initial", NULL, NULL, &cursor));
    testutil_check(cursor->next(cursor));
    testutil_check(cursor->close(cursor));

    testutil_check(session->open_cursor(session, "table:new_table", NULL, NULL, &cursor));
    testutil_check(cursor->next(cursor));
    testutil_check(cursor->close(cursor));

    testutil_check(conn->close(conn, NULL));

    printf("=== Test passed: Create table after skip works ===\n\n");
}

/*
 * write_old_format_marker --
 *     Write an old-format marker (4 fields) to simulate backward compatibility testing.
 */
static void
write_old_format_marker(const char *home, uint32_t file_num, uint32_t offset, int64_t file_size)
{
    FILE *fp_in, *fp_out;
    uint32_t checksum;
    char line[1024];
    char path[1024], tmp_path[1024];
    char marker_data[256], marker_value[256];
    bool found_key, wrote_marker;

    testutil_snprintf(path, sizeof(path), "%s/%s", home, TURTLE_FILE);
    testutil_snprintf(tmp_path, sizeof(tmp_path), "%s/%s.tmp", home, TURTLE_FILE);

    /*
     * Compute checksum over the marker data (excluding checksum itself). This matches the old
     * format checksum computation.
     */
    testutil_snprintf(marker_data, sizeof(marker_data), "file=%u,offset=%u,file_size=%" PRId64,
      file_num, offset, file_size);
    checksum = __wt_checksum(marker_data, strlen(marker_data));

    testutil_snprintf(marker_value, sizeof(marker_value),
      "file=%u,offset=%u,file_size=%" PRId64 ",checksum=%u", file_num, offset, file_size, checksum);

    fp_in = fopen(path, "r");
    testutil_assert(fp_in != NULL);
    fp_out = fopen(tmp_path, "w");
    testutil_assert(fp_out != NULL);

    found_key = false;
    wrote_marker = false;
    while (fgets(line, sizeof(line), fp_in) != NULL) {
        if (found_key && !wrote_marker) {
            /* Replace the marker value line with old format. */
            fprintf(fp_out, "%s\n", marker_value);
            wrote_marker = true;
        } else {
            fputs(line, fp_out);
        }
        if (strstr(line, MARKER_KEY) != NULL)
            found_key = true;
    }

    fclose(fp_in);
    fclose(fp_out);

    testutil_assert(rename(tmp_path, path) == 0);
}

/*
 * test_old_marker_format_compatibility --
 *     Test that new code correctly handles old marker format (4 fields). This simulates upgrading
 *     from an older WiredTiger version.
 */
static void
test_old_marker_format_compatibility(TEST_OPTS *opts)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    int64_t file_size;
    uint32_t file_num, offset, max_fileid, checksum;
    int count, hs_exists, parsed;
    char *marker;
    char new_home[1024];

    printf("=== Test: Old marker format compatibility ===\n");

    /* Create a new home directory for this test. */
    testutil_snprintf(new_home, sizeof(new_home), "%s_oldformat", opts->home);
    testutil_recreate_dir(new_home);

    /* Create database with recovery_skip. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, TABLE_URI, "key_format=i,value_format=S"));

    insert_data(session, 0, 150);
    testutil_check(session->checkpoint(session, NULL));
    testutil_check(conn->close(conn, NULL));

    /* Verify we have a new format marker. */
    testutil_assert(marker_has_extended_fields(new_home));

    /* Get the current marker values. */
    marker = get_marker_value(new_home);
    testutil_assert(marker != NULL);

    parsed = sscanf(marker,
      "file=%" SCNu32 ",offset=%" SCNu32 ",file_size=%" SCNd64 ",max_fileid=%" SCNu32
      ",hs_exists=%d,checksum=%" SCNu32,
      &file_num, &offset, &file_size, &max_fileid, &hs_exists, &checksum);
    testutil_assert(parsed == 6);
    free(marker);

    /* Write an old-format marker (4 fields) with correct checksum. */
    write_old_format_marker(new_home, file_num, offset, file_size);
    printf("Wrote old-format marker: file=%u, offset=%u, file_size=%" PRId64 "\n", file_num, offset,
      file_size);

    /* Verify the marker no longer has extended fields. */
    testutil_assert(!marker_has_extended_fields(new_home));

    /*
     * Reopen - new code should detect old format and fall back to metadata scan. Data should be
     * accessible.
     */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Verify data is intact. */
    count = verify_data(session);
    testutil_assert(count == 150);

    /*
     * Create a new table to verify file ID allocation works after metadata scan fallback.
     */
    testutil_check(session->create(session, "table:after_fallback", "key_format=i,value_format=S"));
    {
        WT_CURSOR *cursor;
        testutil_check(session->open_cursor(session, "table:after_fallback", NULL, NULL, &cursor));
        cursor->set_key(cursor, 1);
        cursor->set_value(cursor, "test_value");
        testutil_check(cursor->insert(cursor));
        testutil_check(cursor->close(cursor));
    }

    testutil_check(session->checkpoint(session, NULL));
    testutil_check(conn->close(conn, NULL));

    /* Reopen and verify both tables exist. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    count = verify_data(session);
    testutil_assert(count == 150);

    {
        WT_CURSOR *cursor;
        testutil_check(session->open_cursor(session, "table:after_fallback", NULL, NULL, &cursor));
        testutil_check(cursor->next(cursor));
        testutil_check(cursor->close(cursor));
    }

    testutil_check(conn->close(conn, NULL));

    printf("=== Test passed: Old marker format handled correctly ===\n\n");
}

/*
 * test_truncated_marker --
 *     Test that a truncated marker triggers full recovery.
 */
static void
test_truncated_marker(TEST_OPTS *opts)
{
    FILE *fp_in, *fp_out;
    WT_CONNECTION *conn;
    WT_SESSION *session;
    int count;
    char line[1024];
    char path[1024], tmp_path[1024];
    char new_home[1024];
    bool found_key, modified;

    printf("=== Test: Truncated marker triggers full recovery ===\n");

    /* Create a new home directory for this test. */
    testutil_snprintf(new_home, sizeof(new_home), "%s_truncated", opts->home);
    testutil_recreate_dir(new_home);

    /* Create database with recovery_skip. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, TABLE_URI, "key_format=i,value_format=S"));

    insert_data(session, 0, 80);
    testutil_check(session->checkpoint(session, NULL));
    testutil_check(conn->close(conn, NULL));

    testutil_assert(marker_exists(new_home));

    /* Truncate the marker by removing the checksum field. */
    testutil_snprintf(path, sizeof(path), "%s/%s", new_home, TURTLE_FILE);
    testutil_snprintf(tmp_path, sizeof(tmp_path), "%s/%s.tmp", new_home, TURTLE_FILE);

    fp_in = fopen(path, "r");
    testutil_assert(fp_in != NULL);
    fp_out = fopen(tmp_path, "w");
    testutil_assert(fp_out != NULL);

    found_key = false;
    modified = false;
    while (fgets(line, sizeof(line), fp_in) != NULL) {
        if (found_key && !modified) {
            /* Truncate at the checksum field. */
            char *checksum_pos = strstr(line, ",checksum=");
            if (checksum_pos != NULL) {
                *checksum_pos = '\n';
                *(checksum_pos + 1) = '\0';
            }
            modified = true;
        }
        fputs(line, fp_out);
        if (strstr(line, MARKER_KEY) != NULL)
            found_key = true;
    }

    fclose(fp_in);
    fclose(fp_out);
    testutil_assert(rename(tmp_path, path) == 0);

    printf("Truncated marker (removed checksum)\n");

    /* Reopen - should fail to parse and do full recovery. */
    testutil_check(wiredtiger_open(new_home, NULL, conn_config_skip, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Verify data is intact. */
    count = verify_data(session);
    testutil_assert(count == 80);

    testutil_check(conn->close(conn, NULL));

    printf("=== Test passed: Truncated marker handled correctly ===\n\n");
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

    /* Extended marker format tests. */
    test_extended_marker_format(opts);
    test_max_fileid_corruption(opts);
    test_hs_exists_corruption(opts);
    test_checksum_corruption(opts);
    test_multiple_tables_fileid(opts);
    test_create_table_after_skip(opts);

    /* Backward/forward compatibility tests. */
    test_old_marker_format_compatibility(opts);
    test_truncated_marker(opts);

    printf("*** All log recovery_skip tests passed ***\n");

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
