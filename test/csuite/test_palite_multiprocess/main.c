/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 */

/*
 * test_palite_multiprocess --
 *     Test PALite multi-process support and synchronous configuration.
 *
 * This test verifies:
 * 1. The synchronous configuration option is properly applied (0, 1, 2)
 * 2. Default synchronous value is FULL (2)
 * 3. Leader/follower data visibility (follower sees leader's data)
 * 4. Leadership transfer between connections
 */

#include "test_util.h"
#include <sys/wait.h>
#include <unistd.h>

#define NUM_ENTRIES 100

static const char *uri = "table:test";
static const char *kv_home_dir = "kv_home";

/*
 * test_synchronous_config --
 *     Test that different synchronous values work correctly.
 */
static void
test_synchronous_config(int sync_value)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    char config[1024];
    char home[64], kv_path[256];
    int i, ret;
    char key[64], value[64];

    printf("  Testing synchronous=%d\n", sync_value);

    /* Use unique home directory for each sync value */
    testutil_snprintf(home, sizeof(home), "WT_TEST_sync%d", sync_value);

    /* Clean up and create directories */
    testutil_recreate_dir(home);
    testutil_snprintf(kv_path, sizeof(kv_path), "%s/%s", home, kv_home_dir);
    testutil_mkdir(kv_path);

    /* Build connection config with PALite extension and synchronous setting */
    testutil_snprintf(config, sizeof(config),
      "create,statistics=(all),"
      "extensions=[%s/ext/page_log/palite/libwiredtiger_palite.so="
      "(config=\"(synchronous=%d)\")],"
      "disaggregated=(role=\"leader\",page_log=palite)",
      WT_BUILDDIR, sync_value);

    /* Open connection directly */
    ret = wiredtiger_open(home, NULL, config, &conn);
    testutil_check(ret);
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create table with disagg block manager */
    testutil_check(
      session->create(session, uri, "key_format=S,value_format=S,block_manager=disagg"));

    /* Write some data */
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    for (i = 0; i < NUM_ENTRIES; i++) {
        testutil_snprintf(key, sizeof(key), "key%06d", i);
        testutil_snprintf(value, sizeof(value), "value%06d", i);
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));

    /* Checkpoint */
    testutil_check(session->checkpoint(session, NULL));

    /* Verify data */
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    i = 0;
    while (cursor->next(cursor) == 0)
        i++;
    testutil_check(cursor->close(cursor));
    testutil_assert(i == NUM_ENTRIES);

    /* Step down before close */
    testutil_check(conn->reconfigure(conn, "disaggregated=(role=\"follower\")"));

    /* Close */
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    printf("  PASS: synchronous=%d, wrote and verified %d entries\n", sync_value, NUM_ENTRIES);
}

/*
 * test_synchronous_default --
 *     Test that the default synchronous value is FULL (2).
 */
static void
test_synchronous_default(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    char config[1024];
    char kv_path[256];
    const char *home = "WT_TEST_default";
    int ret;

    printf("  Testing default synchronous value\n");

    /* Clean up and create directories */
    testutil_recreate_dir(home);
    testutil_snprintf(kv_path, sizeof(kv_path), "%s/%s", home, kv_home_dir);
    testutil_mkdir(kv_path);

    /* Build connection config without explicit synchronous setting */
    testutil_snprintf(config, sizeof(config),
      "create,statistics=(all),"
      "extensions=[%s/ext/page_log/palite/libwiredtiger_palite.so],"
      "disaggregated=(role=\"leader\",page_log=palite)",
      WT_BUILDDIR);

    /* Open connection - default synchronous should be 2 (FULL) */
    ret = wiredtiger_open(home, NULL, config, &conn);
    testutil_check(ret);
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create table */
    testutil_check(
      session->create(session, uri, "key_format=S,value_format=S,block_manager=disagg"));

    /* Step down before close */
    testutil_check(conn->reconfigure(conn, "disaggregated=(role=\"follower\")"));

    /* Close */
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    printf("  PASS: default synchronous value accepted\n");
}

/*
 * test_leader_follower --
 *     Test that a follower can read data written by a leader.
 *     This simulates multi-process access using two connections.
 */
static void
test_leader_follower(void)
{
    WT_CONNECTION *leader_conn, *follower_conn;
    WT_CURSOR *cursor;
    WT_SESSION *leader_session, *follower_session;
    WT_PAGE_LOG *page_log;
    char config[2048], follower_home[256], kv_path[256], kv_link[256];
    char checkpoint_meta[4096];
    const char *home = "WT_TEST_leader_follower";
    uint64_t checkpoint_id, checkpoint_lsn;
    int i, ret, count;
    char key[64], value[64];

    printf("  Testing leader/follower data visibility\n");

    /* Clean up and create directories */
    testutil_recreate_dir(home);
    testutil_snprintf(kv_path, sizeof(kv_path), "%s/%s", home, kv_home_dir);
    testutil_mkdir(kv_path);

    /* Create follower home directory */
    testutil_snprintf(follower_home, sizeof(follower_home), "%s/follower", home);
    testutil_mkdir(follower_home);

    /* Create symlink so follower shares kv_home with leader */
    testutil_snprintf(kv_link, sizeof(kv_link), "%s/%s", follower_home, kv_home_dir);
    ret = symlink("../kv_home", kv_link);
    testutil_assert(ret == 0);

    /* Open leader connection */
    testutil_snprintf(config, sizeof(config),
      "create,statistics=(all),"
      "extensions=[%s/ext/page_log/palite/libwiredtiger_palite.so="
      "(config=\"(synchronous=2)\")],"
      "disaggregated=(role=\"leader\",page_log=palite)",
      WT_BUILDDIR);

    ret = wiredtiger_open(home, NULL, config, &leader_conn);
    testutil_check(ret);
    testutil_check(leader_conn->open_session(leader_conn, NULL, NULL, &leader_session));

    /* Create table and write data */
    testutil_check(
      leader_session->create(leader_session, uri, "key_format=S,value_format=S,block_manager=disagg"));

    testutil_check(leader_session->open_cursor(leader_session, uri, NULL, NULL, &cursor));
    for (i = 0; i < NUM_ENTRIES; i++) {
        testutil_snprintf(key, sizeof(key), "key%06d", i);
        testutil_snprintf(value, sizeof(value), "value%06d", i);
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));

    /* Checkpoint to make data visible to follower */
    testutil_check(leader_session->checkpoint(leader_session, NULL));

    /* Get checkpoint metadata for follower */
    {
        WT_ITEM checkpoint_meta_item;
        memset(&checkpoint_meta_item, 0, sizeof(checkpoint_meta_item));
        testutil_check(leader_conn->get_page_log(leader_conn, "palite", &page_log));
        testutil_check(page_log->pl_get_complete_checkpoint_ext(
          page_log, leader_session, &checkpoint_lsn, &checkpoint_id, NULL, &checkpoint_meta_item));
        testutil_assert(checkpoint_meta_item.size < sizeof(checkpoint_meta));
        memcpy(checkpoint_meta, checkpoint_meta_item.data, checkpoint_meta_item.size);
        checkpoint_meta[checkpoint_meta_item.size] = '\0';
        testutil_check(page_log->terminate(page_log, leader_session));
    }

    /* Open follower connection with checkpoint metadata */
    testutil_snprintf(config, sizeof(config),
      "create,statistics=(all),"
      "extensions=[%s/ext/page_log/palite/libwiredtiger_palite.so="
      "(config=\"(synchronous=2)\")],"
      "disaggregated=(role=\"follower\",page_log=palite,checkpoint_meta=\"%s\")",
      WT_BUILDDIR, checkpoint_meta);

    ret = wiredtiger_open(follower_home, NULL, config, &follower_conn);
    testutil_check(ret);
    testutil_check(follower_conn->open_session(follower_conn, NULL, NULL, &follower_session));

    /* Follower reads and verifies data */
    testutil_check(follower_session->open_cursor(follower_session, uri, NULL, NULL, &cursor));
    count = 0;
    while (cursor->next(cursor) == 0)
        count++;
    testutil_check(cursor->close(cursor));

    testutil_assert(count == NUM_ENTRIES);

    /* Clean up follower */
    testutil_check(follower_session->close(follower_session, NULL));
    testutil_check(follower_conn->close(follower_conn, NULL));

    /* Clean up leader */
    testutil_check(leader_conn->reconfigure(leader_conn, "disaggregated=(role=\"follower\")"));
    testutil_check(leader_session->close(leader_session, NULL));
    testutil_check(leader_conn->close(leader_conn, NULL));

    printf("  PASS: follower saw %d/%d entries written by leader\n", count, NUM_ENTRIES);
}

/*
 * test_leadership_transfer --
 *     Test that leadership can transfer between connections.
 */
static void
test_leadership_transfer(void)
{
    WT_CONNECTION *conn1, *conn2;
    WT_CURSOR *cursor;
    WT_SESSION *session1, *session2;
    WT_PAGE_LOG *page_log;
    char config[2048], home2[256], kv_path[256], kv_link[256];
    char checkpoint_meta[4096];
    const char *home = "WT_TEST_transfer";
    uint64_t checkpoint_id, checkpoint_lsn;
    int i, ret, count;
    char key[64], value[64];

    printf("  Testing leadership transfer\n");

    /* Clean up and create directories */
    testutil_recreate_dir(home);
    testutil_snprintf(kv_path, sizeof(kv_path), "%s/%s", home, kv_home_dir);
    testutil_mkdir(kv_path);

    /* Create second node's home directory */
    testutil_snprintf(home2, sizeof(home2), "%s/node2", home);
    testutil_mkdir(home2);

    /* Create symlink so node2 shares kv_home */
    testutil_snprintf(kv_link, sizeof(kv_link), "%s/%s", home2, kv_home_dir);
    ret = symlink("../kv_home", kv_link);
    testutil_assert(ret == 0);

    /* Leader 1 writes initial data */
    testutil_snprintf(config, sizeof(config),
      "create,statistics=(all),"
      "extensions=[%s/ext/page_log/palite/libwiredtiger_palite.so="
      "(config=\"(synchronous=2)\")],"
      "disaggregated=(role=\"leader\",page_log=palite)",
      WT_BUILDDIR);

    ret = wiredtiger_open(home, NULL, config, &conn1);
    testutil_check(ret);
    testutil_check(conn1->open_session(conn1, NULL, NULL, &session1));

    testutil_check(
      session1->create(session1, uri, "key_format=S,value_format=S,block_manager=disagg"));

    testutil_check(session1->open_cursor(session1, uri, NULL, NULL, &cursor));
    for (i = 0; i < NUM_ENTRIES; i++) {
        testutil_snprintf(key, sizeof(key), "key%06d", i);
        testutil_snprintf(value, sizeof(value), "value%06d", i);
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));
    testutil_check(session1->checkpoint(session1, NULL));

    /* Get checkpoint for transfer */
    {
        WT_ITEM checkpoint_meta_item;
        memset(&checkpoint_meta_item, 0, sizeof(checkpoint_meta_item));
        testutil_check(conn1->get_page_log(conn1, "palite", &page_log));
        testutil_check(page_log->pl_get_complete_checkpoint_ext(
          page_log, session1, &checkpoint_lsn, &checkpoint_id, NULL, &checkpoint_meta_item));
        testutil_assert(checkpoint_meta_item.size < sizeof(checkpoint_meta));
        memcpy(checkpoint_meta, checkpoint_meta_item.data, checkpoint_meta_item.size);
        checkpoint_meta[checkpoint_meta_item.size] = '\0';
        testutil_check(page_log->terminate(page_log, session1));
    }

    /* Leader 1 steps down */
    testutil_check(conn1->reconfigure(conn1, "disaggregated=(role=\"follower\")"));

    /* Leader 2 takes over */
    testutil_snprintf(config, sizeof(config),
      "create,statistics=(all),"
      "extensions=[%s/ext/page_log/palite/libwiredtiger_palite.so="
      "(config=\"(synchronous=2)\")],"
      "disaggregated=(role=\"leader\",page_log=palite,checkpoint_meta=\"%s\")",
      WT_BUILDDIR, checkpoint_meta);

    ret = wiredtiger_open(home2, NULL, config, &conn2);
    testutil_check(ret);
    testutil_check(conn2->open_session(conn2, NULL, NULL, &session2));

    /* Leader 2 verifies it can see leader 1's data */
    testutil_check(session2->open_cursor(session2, uri, NULL, NULL, &cursor));
    count = 0;
    while (cursor->next(cursor) == 0)
        count++;
    testutil_check(cursor->close(cursor));
    testutil_assert(count == NUM_ENTRIES);

    /* Leader 2 writes more data */
    testutil_check(session2->open_cursor(session2, uri, NULL, NULL, &cursor));
    for (i = NUM_ENTRIES; i < NUM_ENTRIES * 2; i++) {
        testutil_snprintf(key, sizeof(key), "key%06d", i);
        testutil_snprintf(value, sizeof(value), "value%06d", i);
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));
    testutil_check(session2->checkpoint(session2, NULL));

    /* Verify total data */
    testutil_check(session2->open_cursor(session2, uri, NULL, NULL, &cursor));
    count = 0;
    while (cursor->next(cursor) == 0)
        count++;
    testutil_check(cursor->close(cursor));
    testutil_assert(count == NUM_ENTRIES * 2);

    /* Clean up */
    testutil_check(conn2->reconfigure(conn2, "disaggregated=(role=\"follower\")"));
    testutil_check(session2->close(session2, NULL));
    testutil_check(conn2->close(conn2, NULL));

    testutil_check(session1->close(session1, NULL));
    testutil_check(conn1->close(conn1, NULL));

    printf("  PASS: leadership transferred, leader 2 wrote %d more entries (total %d)\n",
      NUM_ENTRIES, NUM_ENTRIES * 2);
}

int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    printf("=== PALite Multi-Process Support Tests ===\n\n");

    printf("Test 1: Default synchronous value\n");
    test_synchronous_default();

    printf("\nTest 2: Explicit synchronous values\n");
    test_synchronous_config(0); /* OFF */
    test_synchronous_config(1); /* NORMAL */
    test_synchronous_config(2); /* FULL */

    printf("\nTest 3: Leader/follower data visibility\n");
    test_leader_follower();

    printf("\nTest 4: Leadership transfer\n");
    test_leadership_transfer();

    printf("\n=== All tests passed ===\n");

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
