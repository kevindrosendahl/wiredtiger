/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_conf_source_init_string --
 *     Initialize a config source for string-based config.
 */
void
__wt_conf_source_init_string(WT_CONF_SOURCE *source, const char **cfg)
{
    source->type = WT_CONF_SOURCE_STRING;
    source->u.string.cfg = cfg;
}

/*
 * __wt_conf_source_init_struct --
 *     Initialize a config source for struct-based config.
 */
void
__wt_conf_source_init_struct(
  WT_CONF_SOURCE *source, const WT_OPEN_CONFIG_ARG *args, size_t count)
{
    source->type = WT_CONF_SOURCE_STRUCT;
    source->u.structured.args = args;
    source->u.structured.count = count;
}

/*
 * __conf_source_struct_lookup --
 *     Look up a key in the structured args array.
 *     Linear scan - fast enough for typical configs (5-20 keys).
 */
static int
__conf_source_struct_lookup(
  WT_CONF_SOURCE *source, uint64_t key_id, const WT_OPEN_CONFIG_ARG **argp)
{
    const WT_OPEN_CONFIG_ARG *arg;
    size_t i, count;

    count = source->u.structured.count;

    /* Linear scan through provided args */
    if (count == 0) {
        /* Sentinel-terminated array */
        for (arg = source->u.structured.args; arg->key != WT_OPEN_CONF_KEY_END; arg++) {
            if (arg->key == key_id) {
                *argp = arg;
                return (0);
            }
        }
    } else {
        /* Counted array */
        for (i = 0; i < count; i++) {
            arg = &source->u.structured.args[i];
            if (arg->key == key_id) {
                *argp = arg;
                return (0);
            }
        }
    }

    return (WT_NOTFOUND);
}

/*
 * __wt_conf_source_get_int --
 *     Get an integer config value from the source.
 *     Returns 0 on success, WT_NOTFOUND if key not in struct config.
 *     For struct config, caller should use default if WT_NOTFOUND.
 */
int
__wt_conf_source_get_int(WT_SESSION_IMPL *session, WT_CONF_SOURCE *source, uint64_t key_id,
  const char *key_name, const char *parent_name, int64_t *valuep)
{
    WT_CONFIG_ITEM cval;
    const WT_OPEN_CONFIG_ARG *arg;
    int ret;

    if (source->type == WT_CONF_SOURCE_STRUCT) {
        /* Direct struct lookup - no parsing */
        ret = __conf_source_struct_lookup(source, key_id, &arg);
        if (ret == 0) {
            /* Verify type matches expected (int or bool both use v_int) */
            if (arg->type != WT_OPEN_CONFIG_ARG_INT && arg->type != WT_OPEN_CONFIG_ARG_BOOL)
                WT_RET_MSG(session, EINVAL,
                  "config key %s: expected int/bool type but got type %d", key_name,
                  (int)arg->type);
            *valuep = arg->value.v_int;
            return (0);
        }
        return (ret);
    }

    /* String-based lookup */
    if (parent_name != NULL) {
        /* Nested key: get parent first, then subget */
        WT_CONFIG_ITEM parent_cval;
        WT_RET(__wt_config_gets(session, source->u.string.cfg, parent_name, &parent_cval));
        WT_RET(__wt_config_subgets(session, &parent_cval, key_name, &cval));
    } else {
        WT_RET(__wt_config_gets(session, source->u.string.cfg, key_name, &cval));
    }
    *valuep = cval.val;
    return (0);
}

/*
 * __wt_conf_source_get_boolean --
 *     Get a boolean config value from the source.
 *     Returns 0 on success, WT_NOTFOUND if key not in struct config.
 */
int
__wt_conf_source_get_boolean(WT_SESSION_IMPL *session, WT_CONF_SOURCE *source, uint64_t key_id,
  const char *key_name, const char *parent_name, bool *valuep)
{
    int64_t intval;
    int ret;

    ret = __wt_conf_source_get_int(session, source, key_id, key_name, parent_name, &intval);
    if (ret == 0)
        *valuep = (intval != 0);
    return (ret);
}

/*
 * __wt_conf_source_get_string --
 *     Get a string config value from the source.
 *     Returns 0 on success, WT_NOTFOUND if key not in struct config.
 */
int
__wt_conf_source_get_string(WT_SESSION_IMPL *session, WT_CONF_SOURCE *source, uint64_t key_id,
  const char *key_name, const char *parent_name, WT_CONFIG_ITEM *cval)
{
    const WT_OPEN_CONFIG_ARG *arg;
    int ret;

    if (source->type == WT_CONF_SOURCE_STRUCT) {
        ret = __conf_source_struct_lookup(source, key_id, &arg);
        if (ret == 0) {
            /* Verify type matches expected */
            if (arg->type != WT_OPEN_CONFIG_ARG_STR)
                WT_RET_MSG(session, EINVAL, "config key %s: expected string type but got type %d",
                  key_name, (int)arg->type);
            cval->str = arg->value.v_str.str;
            cval->len = arg->value.v_str.len;
            cval->type = WT_CONFIG_ITEM_STRING;
            cval->val = 0;
            return (0);
        }
        return (ret);
    }

    /* String-based lookup */
    if (parent_name != NULL) {
        WT_CONFIG_ITEM parent_cval;
        WT_RET(__wt_config_gets(session, source->u.string.cfg, parent_name, &parent_cval));
        return __wt_config_subgets(session, &parent_cval, key_name, cval);
    }
    return __wt_config_gets(session, source->u.string.cfg, key_name, cval);
}
