/**
 * @file srbin_shm.h
 * @brief Shared memory cache for binary format data
 *
 * Provides inter-process data caching so multiple sysrepo connections
 * can share deserialized data, avoiding redundant file reads.
 *
 * @copyright
 * Copyright (c) 2026 Vecima Networks Inc.
 *
 * This source code is licensed under BSD-3-Clause License (the "License").
 */

#ifndef _SRBIN_SHM_H
#define _SRBIN_SHM_H

#include <pthread.h>
#include <stdint.h>

#include <libyang/libyang.h>

#include "common_types.h"
#include "sysrepo.h"

/** Default cache size (number of module entries) */
#define SRBIN_SHM_CACHE_SIZE 64

/** Maximum module name length */
#define SRBIN_SHM_MODULE_NAME_MAX 64

/**
 * @brief Cache entry state
 */
enum srbin_shm_entry_state {
    SRBIN_SHM_ENTRY_FREE = 0,    /**< Entry is free */
    SRBIN_SHM_ENTRY_LOADING = 1,  /**< Entry is being loaded */
    SRBIN_SHM_ENTRY_READY = 2,    /**< Entry is ready for use */
    SRBIN_SHM_ENTRY_INVALID = 3   /**< Entry is invalid (schema changed) */
};

/**
 * @brief Shared memory cache entry
 *
 * Each entry represents one cached module datastore.
 */
struct srbin_shm_entry {
    char module_name[SRBIN_SHM_MODULE_NAME_MAX];  /**< Module name */
    sr_datastore_t ds;                              /**< Datastore type */
    uint32_t state;                                 /**< Entry state */
    uint32_t ref_count;                             /**< Reference count */
    uint64_t timestamp;                             /**< Modification time */
    uint64_t access_time;                           /**< Last access time */
    uint64_t access_count;                          /**< Total access count */
    uint64_t data_offset;                           /**< Offset to data in SHM */
    uint64_t data_size;                             /**< Size of cached data */
    uint8_t schema_hash[32];                        /**< Schema hash for validation */
    uint32_t node_count;                            /**< Number of nodes in tree */
    uint32_t _pad;                                  /**< Padding */
};

/**
 * @brief Shared memory cache header
 *
 * Placed at the beginning of the shared memory segment.
 */
struct srbin_shm_header {
    pthread_mutex_t mutex;          /**< Cache mutex */
    uint32_t magic;                 /**< Magic number for validation */
    uint32_t version;               /**< Cache format version */
    uint32_t entry_count;           /**< Number of entries */
    uint32_t capacity;              /**< Cache capacity */
    uint64_t total_size;            /**< Total SHM size */
    uint64_t used_size;             /**< Used space */
    uint64_t hits;                  /**< Cache hits */
    uint64_t misses;                /**< Cache misses */
    uint64_t evictions;             /**< Cache evictions */
    sr_shm_t shm;                   /**< Shared memory info */
    struct srbin_shm_entry entries[SRBIN_SHM_CACHE_SIZE]; /**< Cache entries */
};

/**
 * @brief Initialize shared memory cache
 *
 * Creates or attaches to the shared memory cache.
 *
 * @param[in] shm_prefix SHM prefix for isolation
 * @return 0 on success, -1 on error
 */
int srbf_shm_init(const char *shm_prefix);

/**
 * @brief Clean up shared memory cache
 *
 * Detaches from shared memory. Does not destroy the cache.
 */
void srbf_shm_cleanup(void);

/**
 * @brief Get data from cache
 *
 * @param[in] module_name Module name
 * @param[in] ds Datastore type
 * @param[in] schema_hash Expected schema hash
 * @param[in] ly_ctx libyang context
 * @param[out] data Cached data tree (must be freed by caller)
 * @return 0 on success (cache hit), 1 on miss, -1 on error
 */
int srbf_shm_get(const char *module_name, sr_datastore_t ds,
                 const uint8_t schema_hash[32], struct ly_ctx *ly_ctx,
                 struct lyd_node **data);

/**
 * @brief Put data into cache
 *
 * @param[in] module_name Module name
 * @param[in] ds Datastore type
 * @param[in] schema_hash Schema hash
 * @param[in] data Data tree to cache
 * @param[in] file_timestamp File modification time
 * @return 0 on success, -1 on error
 */
int srbf_shm_set(const char *module_name, sr_datastore_t ds,
                 const uint8_t schema_hash[32], const struct lyd_node *data,
                 uint64_t file_timestamp);

/**
 * @brief Invalidate cache entry
 *
 * @param[in] module_name Module name
 * @param[in] ds Datastore type
 */
void srbf_shm_invalidate(const char *module_name, sr_datastore_t ds);

/**
 * @brief Clear entire cache
 */
void srbf_shm_clear(void);

/**
 * @brief Get cache statistics
 *
 * @param[out] hits Cache hits
 * @param[out] misses Cache misses
 * @param[out] evictions Cache evictions
 * @param[out] hit_ratio Hit ratio (0.0-1.0)
 */
void srbf_shm_stats(uint64_t *hits, uint64_t *misses,
                    uint64_t *evictions, double *hit_ratio);

/**
 * @brief Destroy shared memory cache
 *
 * Removes the shared memory segment. Use with caution.
 */
void srbf_shm_destroy(void);

#endif /* _SRBIN_SHM_H */
