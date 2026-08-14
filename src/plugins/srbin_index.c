/**
 * @file srbin_index.c
 * @brief SRBF XPath index table implementation
 *
 * @copyright
 * Copyright (c) 2026 Vecima Networks Inc.
 *
 * This source code is licensed under BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

#include "srbin_index.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libyang/libyang.h>

#include "srbin_deserialize.h"

/**
 * @brief FNV-1a 64-bit hash constants
 */
#define FNV_PRIME_64 1099511628211ULL
#define FNV_OFFSET_BASIS_64 14695981039346656037ULL

uint64_t
srbf_xpath_hash(const char *xpath)
{
    uint64_t hash = FNV_OFFSET_BASIS_64;

    if (!xpath) {
        return 0;
    }

    /* FNV-1a hash algorithm */
    for (const char *p = xpath; *p; p++) {
        hash ^= (uint64_t)*p;
        hash *= FNV_PRIME_64;
    }

    return hash;
}

int
srbf_idx_init(struct srbf_idx_ctx *ctx, uint32_t capacity)
{
    assert(ctx);

    memset(ctx, 0, sizeof(*ctx));

    if (capacity == 0) {
        capacity = 1024;
    }

    ctx->entries = calloc(capacity, sizeof(struct srbf_idx_entry));
    if (!ctx->entries) {
        return -1;
    }

    ctx->capacity = capacity;
    ctx->count = 0;

    return 0;
}

void
srbf_idx_cleanup(struct srbf_idx_ctx *ctx)
{
    if (ctx) {
        free(ctx->entries);
        ctx->entries = NULL;
        ctx->count = 0;
        ctx->capacity = 0;
    }
}

int
srbf_idx_add(struct srbf_idx_ctx *ctx, const char *xpath, uint64_t offset)
{
    struct srbf_idx_entry *new_entries;

    assert(ctx);

    /* Expand capacity if needed */
    if (ctx->count >= ctx->capacity) {
        ctx->capacity *= 2;
        new_entries = realloc(ctx->entries, ctx->capacity * sizeof(struct srbf_idx_entry));
        if (!new_entries) {
            return -1;
        }
        ctx->entries = new_entries;
    }

    /* Add entry */
    ctx->entries[ctx->count].path_hash = srbf_xpath_hash(xpath);
    ctx->entries[ctx->count].node_offset = offset;
    ctx->count++;

    return 0;
}

static int
srbf_idx_compare(const void *a, const void *b)
{
    const struct srbf_idx_entry *ea = a;
    const struct srbf_idx_entry *eb = b;

    if (ea->path_hash < eb->path_hash) {
        return -1;
    } else if (ea->path_hash > eb->path_hash) {
        return 1;
    }
    return 0;
}

void
srbf_idx_sort(struct srbf_idx_ctx *ctx)
{
    assert(ctx);

    qsort(ctx->entries, ctx->count, sizeof(struct srbf_idx_entry), srbf_idx_compare);
}

API int
srbf_idx_write(int fd, const struct srbf_idx_ctx *ctx)
{
    ssize_t ret;

    assert(ctx);
    assert(fd >= 0);

    ret = write(fd, ctx->entries, ctx->count * sizeof(struct srbf_idx_entry));
    if (ret != (ssize_t)(ctx->count * sizeof(struct srbf_idx_entry))) {
        return -1;
    }

    return 0;
}

/**
 * @brief Binary search in index table
 */
static struct srbf_idx_entry *
srbf_idx_bsearch(struct srbf_idx_entry *entries, uint32_t count, uint64_t hash)
{
    int32_t low = 0, high = count - 1;

    while (low <= high) {
        int32_t mid = (low + high) / 2;
        uint64_t mid_hash = entries[mid].path_hash;

        if (mid_hash == hash) {
            /* Found - check for duplicate hashes (collision) */
            /* In production, would handle collisions properly */
            return &entries[mid];
        } else if (mid_hash < hash) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    return NULL;
}

API int
srbf_load_subtree_by_xpath(int fd, const char *xpath, struct ly_ctx *ly_ctx,
                            const struct lys_module *mod, struct lyd_node **result)
{
    struct srbf_header hdr;
    struct srbf_idx_entry *entry;
    uint64_t hash;
    void *map = NULL;
    size_t map_size = 0;
    struct stat st;
    int ret = -1;

    assert(fd >= 0);
    assert(xpath);
    assert(ly_ctx);
    assert(mod);
    assert(result);

    *result = NULL;

    /* Read header */
    if (srbf_read_header(fd, &hdr) < 0) {
        return -1;
    }

    /* Check if index is present */
    if (!(hdr.flags & SRBF_FLAG_HAS_INDEX)) {
        /* No index available, fall back to full load */
        if (lseek(fd, 0, SEEK_SET) < 0) {
            return -1;
        }
        return srbf_deserialize_tree(fd, ly_ctx, mod, result);
    }

    /* Get file size for memory mapping */
    if (fstat(fd, &st) < 0) {
        return -1;
    }
    map_size = st.st_size;

    /* Memory-map the file */
    map = mmap(NULL, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        return -1;
    }

    /* Calculate XPath hash */
    hash = srbf_xpath_hash(xpath);

    /* Search in index table (entries span [idx_offset, root_offset)) */
    if (hdr.root_offset <= hdr.idx_offset) {
        munmap(map, map_size);
        if (lseek(fd, 0, SEEK_SET) < 0) {
            return -1;
        }
        return srbf_deserialize_tree(fd, ly_ctx, mod, result);
    }

    entry = srbf_idx_bsearch((struct srbf_idx_entry *)((uint8_t *)map + hdr.idx_offset),
                             (uint32_t)((hdr.root_offset - hdr.idx_offset) / sizeof(struct srbf_idx_entry)),
                             hash);

    munmap(map, map_size);
    map = NULL;

    if (entry) {
        if (lseek(fd, 0, SEEK_SET) < 0) {
            return -1;
        }
        /* Found in index - deserialize subtree */
        ret = srbf_deserialize_subtree(fd, entry->node_offset, ly_ctx, mod, result);
    } else {
        /* Not found in index - return empty result */
        ret = 0;
        *result = NULL;
    }

    return ret;
}

/**
 * @brief Build index from data tree
 *
 * This function walks the data tree and builds an index of all
 * significant paths for fast lookup.
 */
/**
 * @brief Helper function to recursively build index
 */
static int
srbf_build_index_recursive(struct lyd_node *node, struct srbf_idx_ctx *ctx)
{
    struct lyd_node *child;
    char xpath[1024];

    assert(node);
    assert(ctx);

    /* Get XPath for this node (lyd_path returns LY_SUCCESS == 0 on success) */
    if (lyd_path(node, LYD_PATH_STD, xpath, sizeof(xpath)) == LY_SUCCESS) {
        /* Add to index (offset will be filled during serialization) */
        srbf_idx_add(ctx, xpath, 0);  /* Offset placeholder */
    }

    /* Recursively process children */
    child = lyd_child(node);
    while (child) {
        srbf_build_index_recursive(child, ctx);
        child = child->next;
    }

    return 0;
}

/**
 * @brief Build index from data tree
 *
 * This function walks the data tree and builds an index of all
 * significant paths for fast lookup.
 */
API int
srbf_build_index(struct lyd_node *root, struct srbf_idx_ctx *ctx)
{
    assert(root);
    assert(ctx);

    /* Initialize index if not already initialized */
    if (ctx->entries == NULL) {
        if (srbf_idx_init(ctx, 1024) < 0) {
            return -1;
        }
    }

    /* Walk the tree and add entries using recursive helper */
    srbf_build_index_recursive(root, ctx);

    /* Sort index for binary search */
    srbf_idx_sort(ctx);

    return 0;
}
