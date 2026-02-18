/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include <atomic>
#include <filesystem>
#include <string>
#include <thread>

#include "wiredtiger.h"
#include "wt_internal.h"
#include "../utils.h"
#include "../wrappers/connection_wrapper.h"
#include "../wrappers/mock_session.h"

/*
 * Unit tests for config-related functions.
 */

TEST_CASE("Parse integer", "[config][parse_int]")
{
    SECTION("No conversion")
    {
        const char *s = "abc";
        char *endptr = nullptr;
        int64_t val = __wti_config_parse_dec(s, strlen(s), &endptr);
        REQUIRE(val == 0);
        REQUIRE(endptr == s);
    }

    SECTION("Boundary conditions")
    {
        const char *max_s = "9223372036854775807"; /* INT64_MAX */
        char *endptr_max = nullptr;
        const int64_t max_val = __wti_config_parse_dec(max_s, strlen(max_s), &endptr_max);
        REQUIRE(max_val == INT64_MAX);
        REQUIRE(endptr_max == max_s + strlen(max_s));

        const char *min_s = "-9223372036854775808"; /* INT64_MIN */
        char *endptr_min = nullptr;
        const int64_t min_val = __wti_config_parse_dec(min_s, strlen(min_s), &endptr_min);
        REQUIRE(min_val == INT64_MIN);
        REQUIRE(endptr_min == min_s + strlen(min_s));
    }

    SECTION("Boundary conditions less than one")
    {
        const char *max_s = "9223372036854775806"; /* INT64_MAX - 1 */
        char *endptr_max = nullptr;
        const int64_t max_val = __wti_config_parse_dec(max_s, strlen(max_s), &endptr_max);
        REQUIRE(max_val == INT64_MAX - 1);
        REQUIRE(endptr_max == max_s + strlen(max_s));

        const char *min_s = "-9223372036854775807"; /* INT64_MIN + 1 */
        char *endptr_min = nullptr;
        const int64_t min_val = __wti_config_parse_dec(min_s, strlen(min_s), &endptr_min);
        REQUIRE(min_val == INT64_MIN + 1);
        REQUIRE(endptr_min == min_s + strlen(min_s));
    }

    SECTION("Out of range")
    {
        const char *overflow_s = "9223372036854775808"; /* INT64_MAX + 1 */
        char *endptr_overflow = nullptr;
        errno = 0;
        const int64_t overflow_val =
          __wti_config_parse_dec(overflow_s, strlen(overflow_s), &endptr_overflow);
        REQUIRE(errno == ERANGE);
        REQUIRE(overflow_val == INT64_MAX);
        REQUIRE(endptr_overflow == overflow_s + strlen(overflow_s));

        const char *underflow_s = "-9223372036854775809"; /* INT64_MIN - 1 */
        char *endptr_underflow = nullptr;
        errno = 0;
        const int64_t underflow_val =
          __wti_config_parse_dec(underflow_s, strlen(underflow_s), &endptr_underflow);
        REQUIRE(errno == ERANGE);
        REQUIRE(underflow_val == INT64_MIN);
        REQUIRE(endptr_underflow == underflow_s + strlen(underflow_s));

        const char *longer_s = "123456789012345678901"; /* Longer than INT64_MAX */
        char *endptr_longer = nullptr;
        errno = 0;
        const int64_t longer_val =
          __wti_config_parse_dec(longer_s, strlen(longer_s), &endptr_longer);
        REQUIRE(errno == ERANGE);
        REQUIRE(longer_val == INT64_MAX);
        REQUIRE(endptr_longer == longer_s + strlen(longer_s));
    }

    SECTION("Limited length")
    {
        const char *s = "123";
        char *endptr = nullptr;
        int64_t val = __wti_config_parse_dec(s, 2, &endptr);
        REQUIRE(val == 12);
        REQUIRE(endptr == s + 2);
    }

    SECTION("Stopping at non-digit")
    {
        const char *s = "123abc";
        char *endptr = nullptr;
        int64_t val = __wti_config_parse_dec(s, strlen(s), &endptr);
        REQUIRE(val == 123);
        REQUIRE(endptr == s + 3);

        const char *blank_s = "   789 ";
        char *endptr_blank = nullptr;
        int64_t val_blank = __wti_config_parse_dec(blank_s, strlen(blank_s), &endptr_blank);
        REQUIRE(val_blank == 789);
        REQUIRE(endptr_blank == blank_s + 6);

        const char *pos_s = "   +123 ";
        char *endptr_pos = nullptr;
        int64_t pos_val = __wti_config_parse_dec(pos_s, strlen(pos_s), &endptr_pos);
        REQUIRE(pos_val == 123);
        REQUIRE(endptr_pos == pos_s + 7);

        const char *neg_s = "   -456 ";
        char *endptr_neg = nullptr;
        int64_t neg_val = __wti_config_parse_dec(neg_s, strlen(neg_s), &endptr_neg);
        REQUIRE(neg_val == -456);
        REQUIRE(endptr_neg == neg_s + 7);
    }

    SECTION("Explicit positive and negative")
    {
        const char *pos_s = "+456";
        char *endptr_pos = nullptr;
        int64_t pos_val = __wti_config_parse_dec(pos_s, strlen(pos_s), &endptr_pos);
        REQUIRE(pos_val == 456);
        REQUIRE(endptr_pos == pos_s + strlen(pos_s));

        const char *neg_s = "-789";
        char *endptr_neg = nullptr;
        int64_t neg_val = __wti_config_parse_dec(neg_s, strlen(neg_s), &endptr_neg);
        REQUIRE(neg_val == -789);
        REQUIRE(endptr_neg == neg_s + strlen(neg_s));

        const char *posz_s = "+0";
        char *endptr_posz = nullptr;
        int64_t posz_val = __wti_config_parse_dec(posz_s, strlen(posz_s), &endptr_posz);
        REQUIRE(posz_val == 0);
        REQUIRE(endptr_posz == posz_s + strlen(posz_s));

        const char *negz_s = "-0";
        char *endptr_negz = nullptr;
        int64_t negz_val = __wti_config_parse_dec(negz_s, strlen(negz_s), &endptr_negz);
        REQUIRE(negz_val == 0);
        REQUIRE(endptr_negz == negz_s + strlen(negz_s));
    }
}

static std::string
collapse_legacy(WT_SESSION_IMPL *session, const char **cfg)
{
    char *collapsed;

    collapsed = nullptr;
    REQUIRE(__wt_config_collapse(session, cfg, &collapsed) == 0);

    std::string result = collapsed == nullptr ? "" : collapsed;
    __wt_free(session, collapsed);
    return (result);
}

static std::string
collapse_fast(WT_SESSION_IMPL *session, const char **cfg, bool &used_fast_path)
{
    char *collapsed;

    collapsed = nullptr;
    used_fast_path = false;
    REQUIRE(__wti_config_collapse_fast(session, cfg, &collapsed, &used_fast_path) == 0);

    std::string result = collapsed == nullptr ? "" : collapsed;
    __wt_free(session, collapsed);
    return (result);
}

struct ws2_test_home_guard {
    std::string home;

    ~ws2_test_home_guard()
    {
        std::error_code ec;
        std::filesystem::remove_all(home, ec);
    }
};

TEST_CASE("Config collapse fast path parity", "[config][collapse_fast]")
{
    std::shared_ptr<mock_session> session_mock = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();

    SECTION("Duplicate projected keys preserve legacy output")
    {
        const char *cfg[] = {"a=1,a=2,b=3", "a=9,b=4", nullptr};
        bool used_fast_path;

        const std::string legacy = collapse_legacy(session, cfg);
        const std::string fast = collapse_fast(session, cfg, used_fast_path);
        REQUIRE(used_fast_path);
        REQUIRE(fast == legacy);
    }

    SECTION("Quoted keys and values match legacy output")
    {
        const char *cfg[] = {"\"a\"=\"x\",b=\"z\"", "\"a\"=\"y\",b=\"q\"", nullptr};
        bool used_fast_path;

        const std::string legacy = collapse_legacy(session, cfg);
        const std::string fast = collapse_fast(session, cfg, used_fast_path);
        REQUIRE(used_fast_path);
        REQUIRE(fast == legacy);
    }

    SECTION("Dotted projected keys use fallback and match legacy output")
    {
        const char *cfg[] = {"log.enabled=false,cache_size=1MB",
          "log=(enabled=true),cache_size=2MB", nullptr};
        bool used_fast_path;

        const std::string legacy = collapse_legacy(session, cfg);
        const std::string fast = collapse_fast(session, cfg, used_fast_path);
        REQUIRE(!used_fast_path);
        REQUIRE(fast == legacy);
    }
}

TEST_CASE("Dhandle config reuse and rebuild counters", "[config][ws2][dhandle]")
{
    const std::string db_home = "test_db_ws2_dhandle";
    const char *uri = "table:cursor_test";
    WT_CONNECTION_IMPL *conn_impl;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    WT_SESSION_IMPL *session_impl;
    int64_t attempts_after_create, attempts_after_verify;
    int64_t attempts_before, hits_after_create, hits_after_verify;
    int64_t rebuilds_after_create, rebuilds_after_alter, rebuilds_before;
    std::error_code ec;

    ws2_test_home_guard cleanup_guard{db_home};
    std::filesystem::remove_all(db_home, ec);
    connection_wrapper conn(db_home, "create,statistics=(all)");
    session_impl = conn.create_session();
    session = &session_impl->iface;
    conn_impl = conn.get_wt_connection_impl();
    cursor = nullptr;

    attempts_before = WT_STAT_CONN_READ(conn_impl->stats, dhandle_config_reuse_attempts);
    rebuilds_before = WT_STAT_CONN_READ(conn_impl->stats, dhandle_config_rebuilds);

    REQUIRE(session->create(session, uri, "key_format=S,value_format=S") == 0);
    REQUIRE(session->open_cursor(session, uri, nullptr, nullptr, &cursor) == 0);
    REQUIRE(cursor->close(cursor) == 0);
    cursor = nullptr;

    attempts_after_create = WT_STAT_CONN_READ(conn_impl->stats, dhandle_config_reuse_attempts);
    hits_after_create = WT_STAT_CONN_READ(conn_impl->stats, dhandle_config_reuse_hits);
    rebuilds_after_create = WT_STAT_CONN_READ(conn_impl->stats, dhandle_config_rebuilds);
    REQUIRE(attempts_after_create > attempts_before);
    REQUIRE(rebuilds_after_create > rebuilds_before);

    /*
     * Verify reopens the handle with unchanged metadata, which should drive a reuse hit on the same
     * dhandle.
     */
    REQUIRE(session->verify(session, uri, nullptr) == 0);
    attempts_after_verify = WT_STAT_CONN_READ(conn_impl->stats, dhandle_config_reuse_attempts);
    hits_after_verify = WT_STAT_CONN_READ(conn_impl->stats, dhandle_config_reuse_hits);
    REQUIRE(attempts_after_verify > attempts_after_create);
    REQUIRE(hits_after_verify > hits_after_create);

    /* Alter table metadata, then reopen to ensure rebuild path is exercised. */
    REQUIRE(session->alter(session, uri, "app_metadata=\"ws2-rebuild\"") == 0);
    REQUIRE(session->open_cursor(session, uri, nullptr, nullptr, &cursor) == 0);
    REQUIRE(cursor->close(cursor) == 0);
    rebuilds_after_alter = WT_STAT_CONN_READ(conn_impl->stats, dhandle_config_rebuilds);
    REQUIRE(rebuilds_after_alter > rebuilds_after_create);
}

TEST_CASE("Dhandle config setup failpoint retry", "[config][ws2][failpoint]")
{
#ifndef HAVE_DIAGNOSTIC
    SUCCEED("Requires diagnostic build");
#else
    const std::string db_home = "test_db_ws2_failpoint";
    const char *uri = "table:cursor_test";
    WT_CONNECTION_IMPL *conn_impl;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    WT_SESSION_IMPL *session_impl;
    int ret;
    std::error_code ec;

    ws2_test_home_guard cleanup_guard{db_home};
    std::filesystem::remove_all(db_home, ec);
    connection_wrapper conn(db_home, "create,statistics=(all)");
    session_impl = conn.create_session();
    session = &session_impl->iface;
    conn_impl = conn.get_wt_connection_impl();
    cursor = nullptr;

    REQUIRE(session->create(session, uri, "key_format=S,value_format=S") == 0);
    REQUIRE(session->open_cursor(session, uri, nullptr, nullptr, &cursor) == 0);
    REQUIRE(cursor->close(cursor) == 0);
    cursor = nullptr;
    REQUIRE(session->alter(session, uri, "app_metadata=\"ws2-failpoint\"") == 0);

    /*
     * Fail once between dhandle cfg allocation and metadata assignment, then verify retry succeeds
     * and does not leave the dhandle in a partially initialized state.
     */
    FLD_SET(conn_impl->debug_flags, WT_CONN_DEBUG_DHANDLE_CONFIG_SET_FAIL);
    ret = session->open_cursor(session, uri, nullptr, nullptr, &cursor);
    REQUIRE(ret == EINVAL);
    REQUIRE(cursor == nullptr);

    REQUIRE(session->open_cursor(session, uri, nullptr, nullptr, &cursor) == 0);
    REQUIRE(cursor->close(cursor) == 0);
#endif
}

TEST_CASE("Strict metadata reuse equality for tiered churn fields", "[config][ws2][tiered_metadata]")
{
    const char *cached = "id=1,block_manager=disagg,last=file:obj.0001,tiers=(file:obj.0001)";
    const char *same = "id=1,block_manager=disagg,last=file:obj.0001,tiers=(file:obj.0001)";
    const char *last_changed = "id=1,block_manager=disagg,last=file:obj.0002,tiers=(file:obj.0001)";
    const char *tiers_changed =
      "id=1,block_manager=disagg,last=file:obj.0001,tiers=(file:obj.0001,file:obj.0002)";

    REQUIRE(__ut_conn_dhandle_metadata_equal_for_reuse(cached, same));
    REQUIRE(!__ut_conn_dhandle_metadata_equal_for_reuse(cached, last_changed));
    REQUIRE(!__ut_conn_dhandle_metadata_equal_for_reuse(cached, tiers_changed));
}

TEST_CASE("Alter checkpoint open concurrency", "[config][ws2][concurrency]")
{
    const std::string db_home = "test_db_ws2_concurrency";
    const char *uri = "table:cursor_test";
    WT_CONNECTION_IMPL *conn_impl;
    WT_CURSOR *cursor;
    WT_SESSION *main_session;
    WT_SESSION *alter_session;
    WT_SESSION *ckpt_session;
    WT_SESSION_IMPL *session_impl;
    std::atomic<int> failure_ret{0};
    std::atomic<int> alter_success{0};
    std::atomic<int> ckpt_success{0};
    std::atomic<int> open_success{0};
    int64_t attempts_after, attempts_before, rebuilds_after, rebuilds_before;
    std::error_code ec;

    ws2_test_home_guard cleanup_guard{db_home};
    std::filesystem::remove_all(db_home, ec);
    connection_wrapper conn(db_home, "create,statistics=(all)");
    session_impl = conn.create_session();
    main_session = &session_impl->iface;
    alter_session = &conn.create_session()->iface;
    ckpt_session = &conn.create_session()->iface;
    conn_impl = conn.get_wt_connection_impl();
    cursor = nullptr;

    REQUIRE(main_session->create(main_session, uri, "key_format=S,value_format=S") == 0);
    REQUIRE(main_session->open_cursor(main_session, uri, nullptr, nullptr, &cursor) == 0);
    REQUIRE(cursor->close(cursor) == 0);

    attempts_before = WT_STAT_CONN_READ(conn_impl->stats, dhandle_config_reuse_attempts);
    rebuilds_before = WT_STAT_CONN_READ(conn_impl->stats, dhandle_config_rebuilds);

    std::thread alter_thread([&]() {
        for (int i = 0; i < 25; ++i) {
            std::string cfg = "app_metadata=\"ws2-concurrency-" + std::to_string(i) + "\"";
            int ret = alter_session->alter(alter_session, uri, cfg.c_str());
            if (ret == 0) {
                ++alter_success;
                continue;
            }
            if (ret == EBUSY || ret == WT_ROLLBACK)
                continue;
            failure_ret.store(ret);
            return;
        }
    });

    std::thread checkpoint_thread([&]() {
        for (int i = 0; i < 25; ++i) {
            int ret = ckpt_session->checkpoint(ckpt_session, nullptr);
            if (ret == 0) {
                ++ckpt_success;
                continue;
            }
            if (ret == EBUSY || ret == WT_ROLLBACK)
                continue;
            failure_ret.store(ret);
            return;
        }
    });

    for (int i = 0; i < 100; ++i) {
        WT_CURSOR *local_cursor = nullptr;
        int ret = main_session->open_cursor(main_session, uri, nullptr, nullptr, &local_cursor);
        if (ret == 0) {
            ++open_success;
            REQUIRE(local_cursor->close(local_cursor) == 0);
            continue;
        }
        if (ret == EBUSY || ret == WT_ROLLBACK)
            continue;
        failure_ret.store(ret);
        break;
    }

    alter_thread.join();
    checkpoint_thread.join();

    REQUIRE(failure_ret.load() == 0);
    REQUIRE(alter_success.load() > 0);
    REQUIRE(ckpt_success.load() > 0);
    REQUIRE(open_success.load() > 0);

    attempts_after = WT_STAT_CONN_READ(conn_impl->stats, dhandle_config_reuse_attempts);
    rebuilds_after = WT_STAT_CONN_READ(conn_impl->stats, dhandle_config_rebuilds);
    REQUIRE(attempts_after > attempts_before);
    REQUIRE(rebuilds_after > rebuilds_before);
}

TEST_CASE("Shared stable URI normalization helper", "[config][ws2][disagg_uri]")
{
    std::shared_ptr<mock_session> session_mock = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();

    SECTION("Stable URI suffix is split into base name and checkpoint id")
    {
        WT_ITEM *name_buf = nullptr;
        const char *checkpoint = nullptr;
        const char *name = "file:test.wt_stable/ckpt.17";

        REQUIRE(__wt_btree_shared_base_name(session, &name, &checkpoint, &name_buf) == 0);
        REQUIRE(std::string(name) == "file:test.wt_stable");
        REQUIRE(std::string(checkpoint) == "ckpt.17");
        __wt_scr_free(session, &name_buf);
    }

    SECTION("Non-stable URI remains unchanged")
    {
        WT_ITEM *name_buf = nullptr;
        const char *checkpoint = "sentinel";
        const char *name = "file:test.wt";

        REQUIRE(__wt_btree_shared_base_name(session, &name, &checkpoint, &name_buf) == 0);
        REQUIRE(std::string(name) == "file:test.wt");
        REQUIRE(std::string(checkpoint) == "sentinel");
        REQUIRE(name_buf == nullptr);
    }
}
