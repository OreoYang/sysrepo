/**
 * @file common_srbin.c
 * @brief Sysrepo Binary Format (SRBF) - Implementation
 *
 * @copyright
 * Copyright (c) 2026 Vecima Networks Inc.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

#include "common_srbin.h"
#include "compat.h"
#include "config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libyang/libyang.h>

#include "common.h"
#include "common_json.h"
#include "sysrepo.h"

#define srpds_name "SRBF"

/**
 * @brief Read bytes from file descriptor
 */
static int
srbf_read_bytes(int fd, void *buf, size_t count)
{
    ssize_t ret;
    size_t have_read = 0;

    while (have_read < count) {
        errno = 0;
        ret = read(fd, (char *)buf + have_read, count - have_read);
        if (ret <= 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        have_read += ret;
    }

    return 0;
}

/**
 * @brief Write bytes to file descriptor
 */
static int
srbf_write_bytes(int fd, const void *buf, size_t count)
{
    ssize_t ret;
    size_t written = 0;

    while (written < count) {
        errno = 0;
        ret = write(fd, (const char *)buf + written, count - written);
        if (ret <= 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        written += ret;
    }

    return 0;
}

int
srbf_is_binary_file(int fd)
{
    char magic[4];
    off_t orig_pos;

    /* Save original position */
    orig_pos = lseek(fd, 0, SEEK_CUR);
    if (orig_pos < 0) {
        return 0;
    }

    /* Seek to beginning and read magic */
    if (lseek(fd, 0, SEEK_SET) < 0) {
        return 0;
    }

    if (srbf_read_bytes(fd, magic, 4) < 0) {
        /* Restore position before returning */
        lseek(fd, orig_pos, SEEK_SET);
        return 0;
    }

    /* Restore position */
    lseek(fd, orig_pos, SEEK_SET);

    return (memcmp(magic, SRBF_MAGIC, 4) == 0) ? 1 : 0;
}

int
srbf_write_header(int fd, const struct srbf_header *hdr)
{
    assert(hdr);
    assert(hdr->magic[0] == 'S' && hdr->magic[1] == 'R' &&
           hdr->magic[2] == 'B' && hdr->magic[3] == 'F');

    /* Write header in little-endian format */
    return srbf_write_bytes(fd, hdr, sizeof(struct srbf_header));
}

int
srbf_read_header(int fd, struct srbf_header *hdr)
{
    int ret;

    assert(hdr);

    /* Read header */
    ret = srbf_read_bytes(fd, hdr, sizeof(struct srbf_header));
    if (ret < 0) {
        return -1;
    }

    /* Validate magic */
    if (memcmp(hdr->magic, SRBF_MAGIC, 4) != 0) {
        return -1;
    }

    /* Validate version */
    if (hdr->version != SRBF_VERSION) {
        return -1;
    }

    return 0;
}

int
srbf_validate_header(const struct srbf_header *hdr, const struct lys_module *mod)
{
    uint8_t calc_hash[32];

    assert(hdr);

    /* Validate magic and version */
    if (memcmp(hdr->magic, SRBF_MAGIC, 4) != 0) {
        return 0;
    }

    if (hdr->version != SRBF_VERSION) {
        return 0;
    }

    /* If module provided, validate schema hash */
    if (mod) {
        if (srbf_calc_schema_hash(mod, calc_hash) < 0) {
            return 0;
        }

        if (memcmp(hdr->schema_hash, calc_hash, 32) != 0) {
            /* Schema mismatch */
            return 0;
        }
    }

    return 1;
}

int
srbf_calc_schema_hash(const struct lys_module *mod, uint8_t hash_out[32])
{
    /* For now, use a simple hash based on module identity.
     * In a full implementation, this would compute SHA256 of the
     * compiled schema or use libyang's schema hash. */
    uint64_t hash;

    assert(mod);
    assert(hash_out);

    /* Simple hash of module name and revision */
    hash = 0;
    for (const char *p = mod->name; *p; p++) {
        hash = hash * 31 + *p;
    }

    /* Store in little-endian format */
    for (int i = 0; i < 8; i++) {
        hash_out[i] = (hash >> (i * 8)) & 0xff;
        hash_out[i + 8] = hash_out[i];
        hash_out[i + 16] = hash_out[i];
        hash_out[i + 24] = hash_out[i];
    }

    return 0;
}

int
srbf_get_path(const char *module_name, sr_datastore_t ds, char **path_out)
{
    const char *ds_suffix;
    const char *base_path;
    char *path = NULL;

    assert(module_name);
    assert(path_out);

    /* Determine datastore suffix */
    switch (ds) {
    case SR_DS_STARTUP:
        ds_suffix = "startup";
        break;
    case SR_DS_RUNNING:
        ds_suffix = "running";
        break;
    case SR_DS_CANDIDATE:
        ds_suffix = "candidate";
        break;
    case SR_DS_FACTORY_DEFAULT:
        ds_suffix = "factory-default";
        break;
    default:
        return -1;
    }

    /* Determine base path - use SR_STARTUP_PATH if set, otherwise use SR_REPO_PATH/data */
    if (SR_STARTUP_PATH[0]) {
        base_path = SR_STARTUP_PATH;
    } else {
        base_path = sr_get_repo_path();
    }

    /* Build path: <base_path>/<module_name>.<datastore>.srbf */
    if (asprintf(&path, "%s/%s.%s%s", base_path, module_name, ds_suffix, SRBF_FILE_EXT) < 0) {
        return -1;
    }

    *path_out = path;
    return 0;
}

int
srbf_get_oper_path(const char *module_name, sr_cid_t cid, uint32_t sid, char **path_out)
{
    char *path = NULL;

    assert(module_name);
    assert(path_out);

    /* Build path for operational data using shared memory path */
    if (asprintf(&path, "%s/%s_%s.operational.%" PRIu32 "-%" PRIu32 "%s",
                 sr_get_shm_path(), sr_get_shm_prefix(), module_name,
                 cid, sid, SRBF_FILE_EXT) < 0) {
        return -1;
    }

    *path_out = path;
    return 0;
}

int
srbf_file_exists(const char *module_name, sr_datastore_t ds)
{
    char *path = NULL;
    struct stat st;

    if (srbf_get_path(module_name, ds, &path) < 0) {
        return 0;
    }

    if (stat(path, &st) < 0) {
        free(path);
        return 0;
    }

    free(path);
    return 1;
}

/**
 * @brief Hash table entry for module data split
 */
struct srbf_mod_split_entry {
    char *mod_name;              /**< Module name */
    struct lyd_node *data;       /**< Data tree for this module */
    struct srbf_mod_split_entry *next; /**< Next entry in hash bucket */
};

/**
 * @brief Module split hash table
 */
struct srbf_mod_split {
    struct srbf_mod_split_entry **buckets; /**< Hash buckets */
    uint32_t bucket_count;        /**< Number of buckets */
    uint32_t entry_count;         /**< Total entries */
    struct ly_ctx *ly_ctx;        /**< libyang context */
};

/**
 * @brief Simple string hash for module names
 */
static uint32_t
srbf_mod_name_hash(const char *str)
{
    uint32_t hash = 5381;

    if (!str) {
        return 0;
    }

    for (const char *p = str; *p; p++) {
        hash = ((hash << 5) + hash) + *p;
    }

    return hash;
}

/**
 * @brief Find or create entry for a module
 */
static struct srbf_mod_split_entry *
srbf_split_get_entry(struct srbf_mod_split *split, const char *mod_name)
{
    uint32_t hash;
    struct srbf_mod_split_entry *entry;

    if (!split || !mod_name) {
        return NULL;
    }

    hash = srbf_mod_name_hash(mod_name) % split->bucket_count;

    /* Search for existing entry */
    for (entry = split->buckets[hash]; entry; entry = entry->next) {
        if (strcmp(entry->mod_name, mod_name) == 0) {
            return entry;
        }
    }

    /* Create new entry */
    entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return NULL;
    }

    entry->mod_name = strdup(mod_name);
    if (!entry->mod_name) {
        free(entry);
        return NULL;
    }

    entry->data = NULL;
    entry->next = split->buckets[hash];
    split->buckets[hash] = entry;
    split->entry_count++;

    return entry;
}

/**
 * @brief Recursively add nodes to module-specific data tree
 */
static int
srbf_split_add_node(struct srbf_mod_split *split, struct lyd_node *node,
                    const struct lys_module *parent_mod)
{
    const struct lys_module *node_mod = NULL;
    struct srbf_mod_split_entry *entry;
    struct lyd_node *new_node = NULL;
    struct lyd_node *child;
    int ret = 0;

    if (!node) {
        return 0;
    }

    /* Determine module ownership */
    if (node->schema) {
        node_mod = lysc_owner_module(node->schema);
        fprintf(stderr, "  DEBUG: Node '%s' has schema, module='%s'\n",
                node->schema->name, node_mod ? node_mod->name : "NULL");
    } else {
        /* Opaque node - use parent's module */
        node_mod = parent_mod;
        fprintf(stderr, "  DEBUG: Opaque node, parent module='%s'\n",
                node_mod ? node_mod->name : "NULL");
    }

    if (!node_mod) {
        /* No module - skip this node */
        fprintf(stderr, "  DEBUG: Skipping node with no module\n");
        return 0;
    }

    /* Get module entry */
    entry = srbf_split_get_entry(split, node_mod->name);
    if (!entry) {
        return -1;
    }

    /* Add node to module's data tree */
    if (entry->data == NULL) {
        /* First node for this module - create new tree */
        if (lyd_dup_single(node, NULL, LYD_DUP_RECURSIVE, &new_node) != LY_SUCCESS) {
            return -1;
        }
        entry->data = new_node;

        /* Debug: print structure */
        fprintf(stderr, "  DEBUG SPLIT: First node for module '%s' is '%s' (children=%d)\n",
                entry->mod_name, LYD_NAME(new_node),
                lyd_child(new_node) != NULL ? 1 : 0);
    } else {
        /* Duplicate and insert as sibling */
        if (lyd_dup_single(node, NULL, LYD_DUP_RECURSIVE, &new_node) != LY_SUCCESS) {
            return -1;
        }
        if (lyd_insert_sibling(entry->data, new_node, &entry->data) != LY_SUCCESS) {
            lyd_free_all(new_node);
            return -1;
        }

        fprintf(stderr, "  DEBUG SPLIT: Added sibling '%s' to module '%s' (children=%d)\n",
                LYD_NAME(new_node), entry->mod_name,
                lyd_child(new_node) != NULL ? 1 : 0);
    }

    return 0;
}

/**
 * @brief Split data tree by module
 *
 * This function separates a multi-module data tree into per-module subtrees.
 * Each module's data is stored in a separate entry in the split structure.
 *
 * @param[in] root Root node of data tree
 * @param[in] ly_ctx libyang context
 * @return Split structure on success, NULL on error
 */
struct srbf_mod_split *
srbf_split_by_module(struct lyd_node *root, struct ly_ctx *ly_ctx)
{
    struct srbf_mod_split *split = NULL;
    struct lyd_node *node;

    if (!root || !ly_ctx) {
        return NULL;
    }

    /* Allocate split structure */
    split = calloc(1, sizeof(*split));
    if (!split) {
        return NULL;
    }

    /* Allocate hash table (64 buckets) */
    split->bucket_count = 64;
    split->buckets = calloc(split->bucket_count, sizeof(struct srbf_mod_split_entry *));
    if (!split->buckets) {
        free(split);
        return NULL;
    }

    split->ly_ctx = ly_ctx;
    split->entry_count = 0;

    /* Split the tree - iterate through all top-level siblings */
    int node_count = 0;
    for (node = root; node; node = node->next) {
        node_count++;
        if (srbf_split_add_node(split, node, NULL) < 0) {
            fprintf(stderr, "  Error: Failed to split node '%s'\n",
                    node->schema ? node->schema->name : "unknown");
            srbf_free_module_split(split);
            return NULL;
        }
    }

    fprintf(stderr, "  DEBUG: Split %d top-level nodes into %u module entries\n",
            node_count, split->entry_count);

    return split;
}

/**
 * @brief Get module data from split structure
 */
struct lyd_node *
srbf_split_get_module_data(struct srbf_mod_split *split, const char *mod_name)
{
    uint32_t hash;
    struct srbf_mod_split_entry *entry;

    if (!split || !mod_name) {
        return NULL;
    }

    hash = srbf_mod_name_hash(mod_name) % split->bucket_count;

    for (entry = split->buckets[hash]; entry; entry = entry->next) {
        if (strcmp(entry->mod_name, mod_name) == 0) {
            return entry->data;
        }
    }

    return NULL;
}

/**
 * @brief Get all module names from split structure
 */
int
srbf_split_get_module_names(struct srbf_mod_split *split, char ***names)
{
    char **name_array = NULL;
    uint32_t i, idx = 0;
    struct srbf_mod_split_entry *entry;

    if (!split || !names) {
        return -1;
    }

    /* Allocate array for module names */
    name_array = calloc(split->entry_count, sizeof(char *));
    if (!name_array) {
        return -1;
    }

    /* Collect all module names */
    for (i = 0; i < split->bucket_count; i++) {
        for (entry = split->buckets[i]; entry; entry = entry->next) {
            name_array[idx++] = strdup(entry->mod_name);
        }
    }

    *names = name_array;
    return (int)split->entry_count;
}

/**
 * @brief Free module split structure
 */
void
srbf_free_module_split(struct srbf_mod_split *split)
{
    uint32_t i;
    struct srbf_mod_split_entry *entry, *next;

    if (!split) {
        return;
    }

    /* Free hash table entries */
    if (split->buckets) {
        for (i = 0; i < split->bucket_count; i++) {
            entry = split->buckets[i];
            while (entry) {
                next = entry->next;
                free(entry->mod_name);
                /* Note: entry->data is not freed - caller's responsibility */
                free(entry);
                entry = next;
            }
        }
        free(split->buckets);
    }

    free(split);
}
