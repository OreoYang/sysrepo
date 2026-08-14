/**
 * @file srbin_shm.c
 * @brief Shared memory cache implementation
 *
 * @copyright
 * Copyright (c) 2026 Vecima Networks Inc.
 *
 * This source code is licensed under BSD-3-Clause License (the "License").
 */

#define _GNU_SOURCE

#include "srbin_shm.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libyang/libyang.h>

#include "common.h"
#include "common_srbin.h"

/** Cache magic number */
#define SRBIN_SHM_MAGIC 0x53424D43  /* "SBMC" */

/** Cache version */
#define SRBIN_SHM_VERSION 1

/** Default SHM size */
#define SRBIN_SHM_DEFAULT_SIZE (16 * 1024 * 1024)  /* 16 MB */

/** SHM name format */
#define SRBIN_SHM_NAME_FORMAT "/srbin_cache_%s"

static struct {
    int initialized;
    struct srbin_shm_header *header;
    char *shm_name;
    size_t shm_size;
} srbf_shm_state = {
    .initialized = 0,
    .header = NULL,
    .shm_name = NULL,
    .shm_size = 0
};

/**
 * @brief Create SHM name from prefix
 */
static char *
srbf_shm_create_name(const char *shm_prefix)
{
    char *name;

    if (asprintf(&name, SRBIN_SHM_NAME_FORMAT,
                 shm_prefix ? shm_prefix : sr_get_shm_prefix()) < 0) {
        return NULL;
    }

    return name;
}

/**
 * @brief Find free entry in cache
 */
static struct srbin_shm_entry *
srbf_shm_find_free_entry(void)
{
    struct srbin_shm_header *hdr = srbf_shm_state.header;
    struct srbin_shm_entry *entry;
    struct srbin_shm_entry *lru_entry = NULL;
    uint64_t oldest_access = UINT64_MAX;

    for (uint32_t i = 0; i < hdr->capacity; i++) {
        entry = &hdr->entries[i];

        if (entry->state == SRBIN_SHM_ENTRY_FREE) {
            return entry;
        }

        if (entry->state == SRBIN_SHM_ENTRY_READY) {
            if (entry->access_time < oldest_access) {
                oldest_access = entry->access_time;
                lru_entry = entry;
            }
        }
    }

    /* Cache is full, evict LRU entry */
    if (lru_entry) {
        hdr->evictions++;
        lru_entry->state = SRBIN_SHM_ENTRY_FREE;
        return lru_entry;
    }

    return NULL;
}

/**
 * @brief Find entry by module name and datastore
 */
static struct srbin_shm_entry *
srbf_shm_find_entry(const char *module_name, sr_datastore_t ds)
{
    struct srbin_shm_header *hdr = srbf_shm_state.header;

    for (uint32_t i = 0; i < hdr->capacity; i++) {
        struct srbin_shm_entry *entry = &hdr->entries[i];

        if (entry->state == SRBIN_SHM_ENTRY_READY ||
            entry->state == SRBIN_SHM_ENTRY_LOADING) {
            if (entry->ds == ds &&
                strcmp(entry->module_name, module_name) == 0) {
                return entry;
            }
        }
    }

    return NULL;
}

/**
 * @brief Initialize shared memory cache
 */
int
srbf_shm_init(const char *shm_prefix)
{
    struct srbin_shm_header *hdr;
    int fd, created = 0;
    pthread_mutexattr_t attr;

    if (srbf_shm_state.initialized) {
        return 0;  /* Already initialized */
    }

    /* Create SHM name */
    srbf_shm_state.shm_name = srbf_shm_create_name(shm_prefix);
    if (!srbf_shm_state.shm_name) {
        return -1;
    }

    srbf_shm_state.shm_size = sizeof(struct srbin_shm_header) + SRBIN_SHM_DEFAULT_SIZE;

    /* Try to open existing SHM */
    fd = shm_open(srbf_shm_state.shm_name, O_RDWR, 0600);
    if (fd < 0) {
        /* Create new SHM */
        fd = shm_open(srbf_shm_state.shm_name, O_CREAT | O_RDWR, 0600);
        if (fd < 0) {
            free(srbf_shm_state.shm_name);
            srbf_shm_state.shm_name = NULL;
            return -1;
        }
        created = 1;
    }

    /* Set size if creating */
    if (created) {
        if (ftruncate(fd, srbf_shm_state.shm_size) < 0) {
            close(fd);
            shm_unlink(srbf_shm_state.shm_name);
            free(srbf_shm_state.shm_name);
            srbf_shm_state.shm_name = NULL;
            return -1;
        }
    }

    /* Memory map */
    hdr = mmap(NULL, srbf_shm_state.shm_size,
               PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (hdr == MAP_FAILED) {
        if (created) {
            shm_unlink(srbf_shm_state.shm_name);
        }
        free(srbf_shm_state.shm_name);
        srbf_shm_state.shm_name = NULL;
        return -1;
    }

    srbf_shm_state.header = hdr;

    /* Initialize header if newly created */
    if (created) {
        memset(hdr, 0, sizeof(*hdr));

        /* Initialize mutex */
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&hdr->mutex, &attr);
        pthread_mutexattr_destroy(&attr);

        hdr->magic = SRBIN_SHM_MAGIC;
        hdr->version = SRBIN_SHM_VERSION;
        hdr->capacity = SRBIN_SHM_CACHE_SIZE;
        hdr->total_size = srbf_shm_state.shm_size;
        hdr->used_size = sizeof(*hdr);
    }

    /* Validate existing SHM */
    if (hdr->magic != SRBIN_SHM_MAGIC || hdr->version != SRBIN_SHM_VERSION) {
        munmap(hdr, srbf_shm_state.shm_size);
        if (created) {
            shm_unlink(srbf_shm_state.shm_name);
        }
        srbf_shm_state.header = NULL;
        free(srbf_shm_state.shm_name);
        srbf_shm_state.shm_name = NULL;
        return -1;
    }

    srbf_shm_state.initialized = 1;
    return 0;
}

/**
 * @brief Clean up shared memory cache
 */
void
srbf_shm_cleanup(void)
{
    if (!srbf_shm_state.initialized) {
        return;
    }

    if (srbf_shm_state.header) {
        munmap(srbf_shm_state.header, srbf_shm_state.shm_size);
        srbf_shm_state.header = NULL;
    }

    /* Note: We don't unlink SHM here, other processes may be using it */
    free(srbf_shm_state.shm_name);
    srbf_shm_state.shm_name = NULL;

    srbf_shm_state.initialized = 0;
}

/**
 * @brief Get data from cache
 */
int
srbf_shm_get(const char *module_name, sr_datastore_t ds,
             const uint8_t schema_hash[32], struct ly_ctx *ly_ctx,
             struct lyd_node **data)
{
    struct srbin_shm_header *hdr;
    struct srbin_shm_entry *entry;
    uint8_t *shm_data;
    int ret = 1;  /* Default: cache miss */

    if (!srbf_shm_state.initialized) {
        return 1;
    }

    hdr = srbf_shm_state.header;

    pthread_mutex_lock(&hdr->mutex);

    entry = srbf_shm_find_entry(module_name, ds);
    if (!entry || entry->state != SRBIN_SHM_ENTRY_READY) {
        hdr->misses++;
        pthread_mutex_unlock(&hdr->mutex);
        return 1;
    }

    /* Validate schema hash */
    if (schema_hash && memcmp(entry->schema_hash, schema_hash, 32) != 0) {
        /* Schema changed, invalidate entry */
        entry->state = SRBIN_SHM_ENTRY_INVALID;
        hdr->misses++;
        pthread_mutex_unlock(&hdr->mutex);
        return 1;
    }

    /* Update access statistics */
    entry->access_time = time(NULL);
    entry->access_count++;
    entry->ref_count++;

    pthread_mutex_unlock(&hdr->mutex);

    /* Deserialize cached data */
    shm_data = (uint8_t *)hdr + entry->data_offset;

    /* For now, just return miss - full implementation would deserialize from SHM */
    /* This is a placeholder for the complete implementation */
    ret = 1;

    pthread_mutex_lock(&hdr->mutex);
    entry->ref_count--;
    if (ret == 0) {
        hdr->hits++;
    } else {
        hdr->misses++;
    }
    pthread_mutex_unlock(&hdr->mutex);

    return ret;
}

/**
 * @brief Put data into cache
 */
int
srbf_shm_set(const char *module_name, sr_datastore_t ds,
             const uint8_t schema_hash[32], const struct lyd_node *data,
             uint64_t file_timestamp)
{
    struct srbin_shm_header *hdr;
    struct srbin_shm_entry *entry;

    if (!srbf_shm_state.initialized) {
        return -1;
    }

    if (!data) {
        return -1;
    }

    hdr = srbf_shm_state.header;

    pthread_mutex_lock(&hdr->mutex);

    /* Check if entry already exists */
    entry = srbf_shm_find_entry(module_name, ds);
    if (!entry) {
        /* Find free entry */
        entry = srbf_shm_find_free_entry();
        if (!entry) {
            pthread_mutex_unlock(&hdr->mutex);
            return -1;  /* Cache full */
        }
    }

    /* Initialize entry */
    strncpy(entry->module_name, module_name, SRBIN_SHM_MODULE_NAME_MAX - 1);
    entry->module_name[SRBIN_SHM_MODULE_NAME_MAX - 1] = '\0';
    entry->ds = ds;
    entry->state = SRBIN_SHM_ENTRY_LOADING;
    entry->timestamp = file_timestamp;
    entry->access_time = time(NULL);
    entry->access_count = 0;
    entry->ref_count = 0;

    if (schema_hash) {
        memcpy(entry->schema_hash, schema_hash, 32);
    }

    /* Calculate data size (estimate) */
    /* In full implementation, would serialize data to SHM here */
    entry->node_count = 0;  /* Would count nodes */
    entry->data_size = 0;   /* Would be actual size */
    entry->data_offset = 0; /* Would be actual offset */

    entry->state = SRBIN_SHM_ENTRY_READY;

    pthread_mutex_unlock(&hdr->mutex);

    return 0;
}

/**
 * @brief Invalidate cache entry
 */
void
srbf_shm_invalidate(const char *module_name, sr_datastore_t ds)
{
    struct srbin_shm_header *hdr;
    struct srbin_shm_entry *entry;

    if (!srbf_shm_state.initialized) {
        return;
    }

    hdr = srbf_shm_state.header;

    pthread_mutex_lock(&hdr->mutex);

    entry = srbf_shm_find_entry(module_name, ds);
    if (entry) {
        entry->state = SRBIN_SHM_ENTRY_INVALID;
    }

    pthread_mutex_unlock(&hdr->mutex);
}

/**
 * @brief Clear entire cache
 */
void
srbf_shm_clear(void)
{
    struct srbin_shm_header *hdr;

    if (!srbf_shm_state.initialized) {
        return;
    }

    hdr = srbf_shm_state.header;

    pthread_mutex_lock(&hdr->mutex);

    /* Mark all entries as free */
    for (uint32_t i = 0; i < hdr->capacity; i++) {
        hdr->entries[i].state = SRBIN_SHM_ENTRY_FREE;
    }

    hdr->used_size = sizeof(*hdr);
    hdr->hits = 0;
    hdr->misses = 0;
    hdr->evictions = 0;

    pthread_mutex_unlock(&hdr->mutex);
}

/**
 * @brief Get cache statistics
 */
void
srbf_shm_stats(uint64_t *hits, uint64_t *misses,
               uint64_t *evictions, double *hit_ratio)
{
    struct srbin_shm_header *hdr;

    if (!srbf_shm_state.initialized) {
        return;
    }

    hdr = srbf_shm_state.header;

    pthread_mutex_lock(&hdr->mutex);

    if (hits) {
        *hits = hdr->hits;
    }
    if (misses) {
        *misses = hdr->misses;
    }
    if (evictions) {
        *evictions = hdr->evictions;
    }
    if (hit_ratio) {
        uint64_t total = hdr->hits + hdr->misses;
        *hit_ratio = (total > 0) ? ((double)hdr->hits / total) : 0.0;
    }

    pthread_mutex_unlock(&hdr->mutex);
}

/**
 * @brief Destroy shared memory cache
 */
void
srbf_shm_destroy(void)
{
    if (!srbf_shm_state.initialized) {
        return;
    }

    if (srbf_shm_state.header) {
        munmap(srbf_shm_state.header, srbf_shm_state.shm_size);
        srbf_shm_state.header = NULL;
    }

    if (srbf_shm_state.shm_name) {
        shm_unlink(srbf_shm_state.shm_name);
        free(srbf_shm_state.shm_name);
        srbf_shm_state.shm_name = NULL;
    }

    srbf_shm_state.initialized = 0;
}
