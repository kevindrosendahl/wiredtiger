/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * WT_CONF_SOURCE_TYPE --
 *     Indicates where config values should be read from.
 */
typedef enum {
    WT_CONF_SOURCE_STRING, /* Traditional string-based config */
    WT_CONF_SOURCE_STRUCT  /* Structured config args */
} WT_CONF_SOURCE_TYPE;

/* Forward declaration and typedef for the struct */
struct __wt_conf_source;
typedef struct __wt_conf_source WT_CONF_SOURCE;

/*
 * WT_CONF_SOURCE --
 *     Abstraction for reading configuration values from different sources.
 *
 * Design note: We intentionally avoid pre-building lookup indices.
 * For typical configs (5-20 keys), linear scan is fast enough and
 * avoids memory overhead. If profiling shows this is a bottleneck,
 * we can add lazy hash table construction for large configs.
 *
 * TRADEOFF: This abstraction introduces dual code paths in config-reading
 * functions. This is a maintenance cost accepted in exchange for the
 * performance benefit of avoiding string parsing. An alternative would
 * be to build a config string internally and delegate to existing code,
 * but that would not achieve the performance goal.
 */
struct __wt_conf_source {
    WT_CONF_SOURCE_TYPE type;

    union {
        /* For string-based config */
        struct {
            const char **cfg; /* Config string stack */
        } string;

        /* For struct-based config */
        struct {
            const WT_OPEN_CONFIG_ARG *args;
            size_t count; /* 0 means sentinel-terminated */
        } structured;
    } u;
};
