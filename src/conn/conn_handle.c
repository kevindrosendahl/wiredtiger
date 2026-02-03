/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
#include "wiredtiger_open_conf.h"

/*
 * __wti_connection_init --
 *     Structure initialization for a just-created WT_CONNECTION_IMPL handle.
 */
int
__wti_connection_init(WT_CONNECTION_IMPL *conn)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = conn->default_session;

    TAILQ_INIT(&conn->chunkcache_metadataqh); /* Chunk cache metadata work unit list */
    TAILQ_INIT(&conn->dhqh);                  /* Data handle list */
    TAILQ_INIT(&conn->dlhqh);                 /* Library list */
    TAILQ_INIT(&conn->dsrcqh);                /* Data source list */
    TAILQ_INIT(&conn->fhqh);                  /* File list */
    TAILQ_INIT(&conn->collqh);                /* Collator list */
    TAILQ_INIT(&conn->compqh);                /* Compressor list */
    TAILQ_INIT(&conn->encryptqh);             /* Encryptor list */
    TAILQ_INIT(&conn->pagelogqh);             /* Page log list */
    TAILQ_INIT(&conn->storagesrcqh);          /* Storage source list */
    TAILQ_INIT(&conn->tieredqh);              /* Tiered work unit list */
    TAILQ_INIT(&conn->pfqh);                  /* Pre-fetch reference list */

    /* Disaggregated storage. */
    TAILQ_INIT(&conn->disaggregated_storage.copy_metadata_qh);

    /* Random numbers. */
    __wt_session_rng_init_once(session);

    /* Configuration. */
    WT_RET(__wt_conn_config_init(session));

    /* Statistics. */
    WT_RET(__wt_stat_connection_init(session, conn));

    /* Spinlocks. */
    WT_RET(__wt_spin_init(session, &conn->api_lock, "api"));
    WT_SPIN_INIT_TRACKED(session, &conn->checkpoint_lock, checkpoint);
    WT_RET(__wt_spin_init(session, &conn->background_compact.lock, "background compact"));
    WT_RET(__wt_spin_init(session, &conn->chunkcache_metadata_lock, "chunk cache metadata"));
    WT_RET(__wt_spin_init(
      session, &conn->disaggregated_storage.copy_metadata_lock, "copy shared metadata"));
    WT_RET(__wt_spin_init(session, &conn->encryptor_lock, "encryptor"));
    WT_RET(__wt_spin_init(session, &conn->fh_lock, "file list"));
    WT_RET(__wt_spin_init(session, &conn->flush_tier_lock, "flush tier"));
    WT_SPIN_INIT_TRACKED(session, &conn->metadata_lock, metadata);
    WT_RET(__wt_spin_init(session, &conn->reconfig_lock, "reconfigure"));
    WT_SPIN_INIT_SESSION_TRACKED(session, &conn->schema_lock, schema);
    WT_RET(__wt_spin_init(session, &conn->storage_lock, "tiered storage"));
    WT_RET(__wt_spin_init(session, &conn->tiered_lock, "tiered work unit list"));
    WT_RET(__wt_spin_init(session, &conn->turtle_lock, "turtle file"));
    WT_RET(__wt_spin_init(session, &conn->prefetch_lock, "prefetch"));

    /* Read-write locks */
    WT_RET(__wt_rwlock_init(session, &conn->log_mgr.debug_log_retention_lock));
    WT_RWLOCK_INIT_SESSION_TRACKED(session, &conn->dhandle_lock, dhandle);
    WT_RET(__wt_rwlock_init(session, &conn->hot_backup_lock));
    WT_RWLOCK_INIT_TRACKED(session, &conn->table_lock, table);

    /* Initialize the generation manager. */
    __wt_gen_init(session);

    /*
     * Block manager. XXX If there's ever a second block manager, we'll want to make this more
     * opaque, but for now this is simpler.
     */
    WT_RET(__wt_spin_init(session, &conn->block_lock, "block manager"));
    TAILQ_INIT(&conn->blockqh); /* Block manager list */

    __wt_checkpoint_timer_stats_clear(session);

err:
    return (ret);
}

/*
 * __wti_connection_destroy --
 *     Destroy the connection's underlying WT_CONNECTION_IMPL structure.
 */
void
__wti_connection_destroy(WT_CONNECTION_IMPL *conn)
{
    WT_SESSION_IMPL *session;

    /* Check there's something to destroy. */
    if (conn == NULL)
        return;

    session = conn->default_session;

    /* Remove from the list of connections. */
    __wt_spin_lock(session, &__wt_process.spinlock);
    TAILQ_REMOVE(&__wt_process.connqh, conn, q);
    __wt_spin_unlock(session, &__wt_process.spinlock);

    /* Configuration */
    __wt_conn_config_discard(session); /* configuration */

    __wt_conn_foc_discard(session); /* free-on-close */

    __wt_spin_destroy(session, &conn->api_lock);
    __wt_spin_destroy(session, &conn->background_compact.lock);
    __wt_spin_destroy(session, &conn->block_lock);
    __wt_spin_destroy(session, &conn->checkpoint_lock);
    __wt_spin_destroy(session, &conn->chunkcache_metadata_lock);
    __wt_spin_destroy(session, &conn->disaggregated_storage.copy_metadata_lock);
    __wt_rwlock_destroy(session, &conn->log_mgr.debug_log_retention_lock);
    __wt_rwlock_destroy(session, &conn->dhandle_lock);
    __wt_spin_destroy(session, &conn->encryptor_lock);
    __wt_spin_destroy(session, &conn->fh_lock);
    __wt_spin_destroy(session, &conn->flush_tier_lock);
    __wt_rwlock_destroy(session, &conn->hot_backup_lock);
    __wt_spin_destroy(session, &conn->metadata_lock);
    __wt_spin_destroy(session, &conn->reconfig_lock);
    __wt_spin_destroy(session, &conn->schema_lock);
    __wt_spin_destroy(session, &conn->storage_lock);
    __wt_rwlock_destroy(session, &conn->table_lock);
    __wt_spin_destroy(session, &conn->tiered_lock);
    __wt_spin_destroy(session, &conn->turtle_lock);
    __wt_spin_destroy(session, &conn->prefetch_lock);

    /* Free allocated hash buckets. */
    __wt_free(session, conn->blockhash);
    __wt_free(session, conn->dh_bucket_count);
    __wt_free(session, conn->dhhash);
    __wt_free(session, conn->fhhash);

    /* Free allocated recovered checkpoint snapshot memory */
    __wt_free(session, conn->recovery_ckpt_snapshot);

    /* Free allocated memory. */
    __wt_free(session, conn->cfg);
    __wt_free(session, conn->debug_ckpt);
    __wt_free(session, conn->error_prefix);
    __wt_free(session, conn->home);
    __wt_free(session, WT_CONN_SESSIONS_GET(conn));

    /*
     * Free struct config source BEFORE __wt_stat_connection_discard. The __wt_free macro updates
     * memory statistics, which requires the stats array to still be valid.
     */
    /* Free struct config source if present (from wiredtiger_open_ex) */
    if (conn->conf_source != NULL) {
        /*
         * Free any copied string values in the args array.
         *
         * Note: We cast away const from args and v_str.str below. This is intentional. The public
         * WT_OPEN_CONFIG_ARG struct uses 'const char *' for v_str.str to signal to users that they
         * shouldn't modify the string. However, during wiredtiger_open_ex we copy user strings via
         * __wt_strndup and store them in this const field. When freeing, we must cast away const.
         * This is a standard C idiom for "const in API, mutable internally" and is safe because we
         * allocated this memory ourselves.
         */
        if (conn->conf_source->type == WT_CONF_SOURCE_STRUCT &&
          conn->conf_source->u.structured.args != NULL) {
            WT_OPEN_CONFIG_ARG *args = (WT_OPEN_CONFIG_ARG *)conn->conf_source->u.structured.args;
            size_t count = conn->conf_source->u.structured.count;
            size_t i;

            /* If count is 0, it's sentinel-terminated */
            if (count == 0) {
                for (i = 0; args[i].key != WT_OPEN_CONF_KEY_END; i++) {
                    if (args[i].type == WT_OPEN_CONFIG_ARG_STR)
                        __wt_free(session, args[i].value.v_str.str);
                }
            } else {
                for (i = 0; i < count; i++) {
                    if (args[i].type == WT_OPEN_CONFIG_ARG_STR)
                        __wt_free(session, args[i].value.v_str.str);
                }
            }
            __wt_free(session, args);
        }
        __wt_free(session, conn->conf_source);
    }

    /* Now safe to discard stats - all __wt_free calls are done */
    __wt_stat_connection_discard(session, conn);

    __wt_free(NULL, conn);
}
