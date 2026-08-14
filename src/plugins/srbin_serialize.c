/**
 * @file srbin_serialize.c
 * @brief SRBF serialization (write path) - Converts libyang data tree to binary format
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

#include "common_srbin.h"
#include "srbin_serialize.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libyang/libyang.h>

#include "common.h"
#include "srbin_index.h"

#define srpds_name "SRBF"

/**
 * @brief String table entry for tracking deduplicated strings
 */
struct srbf_strtbl_entry {
    char *string;
    uint64_t offset;
    struct srbf_strtbl_entry *next;
    struct srbf_strtbl_entry *order_next;  /* Next in insertion order */
};

/**
 * @brief Context for serialization
 */
struct srbf_serctx {
    struct srbf_buf buf;       /**< Serialization buffer */
    struct srbf_strtbl_entry **strtbl_hash; /**< String hash table */
    uint32_t strtbl_size;      /**< String table size */
    uint32_t strtbl_count;     /**< Number of strings in table */
    uint64_t node_count;       /**< Number of nodes serialized */
    uint64_t strtbl_offset;    /**< Offset to string table */
    struct lyd_node *root;     /**< Root node being serialized */
    const struct lys_module *file_mod; /**< Module that owns this file */
    struct srbf_strtbl_entry *strtbl_head;  /**< Head of insertion order list */
    struct srbf_strtbl_entry *strtbl_tail;  /**< Tail of insertion order list */
    struct srbf_idx_ctx *idx_ctx; /**< When non-NULL, record XPath hash + file offset per node */
};

/**
 * @brief Simple string hash for hash table
 */
static uint32_t
srbf_str_hash(const char *str)
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
 * @brief Initialize serialization context
 */
static int
srbf_serctx_init(struct srbf_serctx *ctx, uint64_t initial_capacity)
{
    assert(ctx);

    memset(ctx, 0, sizeof(*ctx));

    /* Allocate buffer */
    ctx->buf.capacity = initial_capacity;
    ctx->buf.data = calloc(1, ctx->buf.capacity);
    if (!ctx->buf.data) {
        return -1;
    }

    /* Allocate string hash table */
    ctx->strtbl_size = 1024;
    ctx->strtbl_hash = calloc(ctx->strtbl_size, sizeof(struct srbf_strtbl_entry *));
    if (!ctx->strtbl_hash) {
        free(ctx->buf.data);
        return -1;
    }

    return 0;
}

/**
 * @brief Clean up serialization context
 */
static void
srbf_serctx_cleanup(struct srbf_serctx *ctx)
{
    if (ctx) {
        /* Free string table entries */
        if (ctx->strtbl_hash) {
            for (uint32_t i = 0; i < ctx->strtbl_size; i++) {
                struct srbf_strtbl_entry *entry = ctx->strtbl_hash[i];
                while (entry) {
                    struct srbf_strtbl_entry *next = entry->next;
                    free(entry->string);
                    free(entry);
                    entry = next;
                }
            }
            free(ctx->strtbl_hash);
        }

        /* Free buffer */
        free(ctx->buf.data);
    }
}

/**
 * @brief Ensure buffer has enough capacity
 */
static int
srbf_buf_ensure(struct srbf_buf *buf, uint64_t needed)
{
    uint64_t new_capacity;

    if (buf->size + needed <= buf->capacity) {
        return 0;
    }

    /* Double capacity or allocate needed space */
    new_capacity = buf->capacity * 2;
    if (new_capacity < buf->size + needed) {
        new_capacity = buf->size + needed + 4096;
    }

    uint8_t *new_data = realloc(buf->data, new_capacity);
    if (!new_data) {
        return -1;
    }

    buf->data = new_data;
    buf->capacity = new_capacity;

    return 0;
}

/**
 * @brief Add string to string table (deduplicated)
 */
static uint64_t
srbf_strtbl_add(struct srbf_serctx *ctx, const char *str)
{
    struct srbf_strtbl_entry *entry;
    uint32_t hash;

    if (!str || !*str) {
        return 0;
    }

    hash = srbf_str_hash(str) % ctx->strtbl_size;

    /* Check if string already exists */
    for (entry = ctx->strtbl_hash[hash]; entry; entry = entry->next) {
        if (strcmp(entry->string, str) == 0) {
            return entry->offset;
        }
    }

    /* Add new string */
    entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return 0;
    }

    entry->string = strdup(str);
    if (!entry->string) {
        free(entry);
        return 0;
    }

    entry->offset = ctx->strtbl_offset;
    entry->next = ctx->strtbl_hash[hash];
    ctx->strtbl_hash[hash] = entry;

    /* Add to insertion order list */
    entry->order_next = NULL;
    if (ctx->strtbl_tail) {
        ctx->strtbl_tail->order_next = entry;
    } else {
        ctx->strtbl_head = entry;
    }
    ctx->strtbl_tail = entry;

    /* Update offset for next string */
    ctx->strtbl_offset += sizeof(uint32_t) + strlen(str);

    /* Increment string count */
    ctx->strtbl_count++;

    return entry->offset;
}

/**
 * @brief Write data to buffer
 */
static int
srbf_buf_write(struct srbf_buf *buf, const void *data, uint64_t size)
{
    if (srbf_buf_ensure(buf, size) < 0) {
        return -1;
    }

    memcpy(buf->data + buf->size, data, size);
    buf->size += size;

    return 0;
}

/**
 * @brief Count nodes in the same order as srbf_serialize_node() visits them.
 */
static uint64_t
srbf_count_serialize_nodes(struct lyd_node *node)
{
    uint64_t n = 1;
    struct lyd_node *ch = lyd_child(node);

    if (ch) {
        n += srbf_count_serialize_nodes(ch);
    }
    if (node->next && node->parent != node) {
        n += srbf_count_serialize_nodes(node->next);
    }

    return n;
}

/**
 * @brief Count all nodes for every top-level sibling chain entry point.
 */
static uint64_t
srbf_count_serialize_tree(struct lyd_node *root)
{
    uint64_t t = 0;
    struct lyd_node *node;

    for (node = root; node; node = node->next) {
        t += srbf_count_serialize_nodes(node);
    }

    return t;
}

/**
 * @brief Serialize a single node
 *
 * This is the core serialization function that converts a libyang node
 * into the binary format. Handles both regular and opaque nodes.
 */
static uint64_t
srbf_serialize_node(struct srbf_serctx *ctx, struct lyd_node *node)
{
    struct srbf_node bin_node;
    uint64_t node_offset, child_offset = 0, sibling_offset = 0;
    const char *str_val = NULL;
    int is_opaque = 0;
    char xpath_buf[1024];

    /* Save current offset for this node */
    node_offset = ctx->buf.size;

    if (ctx->idx_ctx) {
        if (lyd_path(node, LYD_PATH_STD, xpath_buf, sizeof(xpath_buf)) == LY_SUCCESS) {
            if (srbf_idx_add(ctx->idx_ctx, xpath_buf, node_offset) < 0) {
                return 0;
            }
        } else {
            /* Fallback so index entry count matches serialized node count */
            snprintf(xpath_buf, sizeof(xpath_buf), "/@srbf-offset=%" PRIu64, node_offset);
            if (srbf_idx_add(ctx->idx_ctx, xpath_buf, node_offset) < 0) {
                return 0;
            }
        }
    }

    /* Initialize binary node */
    memset(&bin_node, 0, sizeof(bin_node));

    /* Check if this is an opaque node (no schema) */
    is_opaque = (node->schema == NULL);

    /* Set node type - use LYS_UNKNOWN for opaque nodes */
    if (is_opaque) {
        bin_node.node_type = LYS_UNKNOWN;
    } else {
        bin_node.node_type = (uint16_t)node->schema->nodetype;
    }

    /* Set default flag */
    bin_node.dflt_flag = (node->flags & LYD_DEFAULT) ? 1 : 0;

    /*
     * String-table offsets for module + local node name (SRBF v2).
     * mod_name_off == 0: owning module is the file module (ctx->file_mod).
     * node_name_off: always set for opaque; set for schema nodes.
     */
    bin_node.mod_name_off = 0;
    bin_node.node_name_off = 0;
    if (is_opaque) {
        const char *node_name = LYD_NAME(node);

        if (node_name) {
            bin_node.node_name_off = (uint32_t)srbf_strtbl_add(ctx, node_name);
        }
    } else {
        const struct lysc_node *schema = node->schema;
        const struct lys_module *node_mod = lysc_owner_module(schema);

        bin_node.node_name_off = (uint32_t)srbf_strtbl_add(ctx, schema->name);
        if (node_mod && ctx->file_mod && (node_mod != ctx->file_mod)) {
            bin_node.mod_name_off = (uint32_t)srbf_strtbl_add(ctx, node_mod->name);
        }
    }

    /* Set value type and value */
    if (is_opaque) {
        /* Opaque node - store value string */
        struct lyd_node_opaq *opaq = (struct lyd_node_opaq *)node;
        bin_node.value_type = LY_TYPE_STRING;  /* Opaque values are strings */

        /* Get the value string */
        if (opaq->value) {
            bin_node.value_offset = srbf_strtbl_add(ctx, opaq->value);
        } else {
            bin_node.value_offset = 0;
        }
    } else {
        /* Regular node with schema */
        const struct lysc_node *schema = node->schema;

        switch (schema->nodetype) {
        case LYS_LEAF:
        case LYS_LEAFLIST: {
            struct lyd_node_term *term = (struct lyd_node_term *)node;
            bin_node.value_type = term->value.realtype->basetype;
            /* Get string value and add to string table */
            str_val = lyd_get_value(node);
            if (str_val) {
                bin_node.value_offset = srbf_strtbl_add(ctx, str_val);
            }
            break;
        }
        default:
            bin_node.value_type = 0;
            bin_node.value_offset = 0;
            break;
        }
    }

    /* Reserve space for child and sibling offsets (will be filled later) */
    bin_node.first_child = 0;
    bin_node.next_sibling = 0;
    bin_node.parent = 0;

    /* Write node to buffer */
    if (srbf_buf_write(&ctx->buf, &bin_node, sizeof(bin_node)) < 0) {
        return 0;
    }

    ctx->node_count++;

    /* Serialize children */
    struct lyd_node *child = lyd_child(node);
    if (child) {
        child_offset = srbf_serialize_node(ctx, child);
        /* Update first_child offset in the node we just wrote */
        struct srbf_node *written_node = (struct srbf_node *)(ctx->buf.data + node_offset);
        written_node->first_child = child_offset;
    }

    /* Serialize siblings */
    if (node->next && node->parent != node) {
        sibling_offset = srbf_serialize_node(ctx, node->next);
        /* Update next_sibling offset in the node we just wrote */
        struct srbf_node *written_node = (struct srbf_node *)(ctx->buf.data + node_offset);
        written_node->next_sibling = sibling_offset;
    }

    return node_offset;
}

/**
 * @brief Serialize data tree to binary format
 *
 * On-disk layout: [header][index table][node records][string table].
 * Index entries (path_hash, node_offset) are sorted by path_hash for binary search.
 */
API int
srbf_serialize_tree(const struct lyd_node *root, int fd, const struct lys_module *mod)
{
    struct srbf_serctx ctx;
    struct srbf_header hdr;
    uint64_t strtbl_start;
    const uint64_t hdr_sz = sizeof(struct srbf_header);
    uint64_t total_nodes, index_bytes;
    const uint64_t idx_offset = hdr_sz;
    uint64_t first_offset = 0;
    struct srbf_idx_ctx idx_ctx;
    int ret = -1;
    int ncnt = 0;
    struct lyd_node *node;

    memset(&idx_ctx, 0, sizeof(idx_ctx));

    assert(root);
    assert(fd >= 0);
    assert(mod);

    /* Initialize serialization context */
    if (srbf_serctx_init(&ctx, 65536) < 0) {
        return -1;
    }

    ctx.root = (struct lyd_node *)root;
    ctx.file_mod = mod;  /* Store file's module for cross-module detection */
    ctx.idx_ctx = NULL;

    /* Reserve space for header (will be filled at end) */
    if (srbf_buf_write(&ctx.buf, &hdr, sizeof(hdr)) < 0) {
        goto cleanup;
    }

    total_nodes = srbf_count_serialize_tree(ctx.root);
    if (total_nodes == 0ULL) {
        fprintf(stderr, "  Error: empty tree for SRBF serialization\n");
        goto cleanup;
    }

    index_bytes = total_nodes * sizeof(struct srbf_idx_entry);
    if (srbf_buf_ensure(&ctx.buf, index_bytes) < 0) {
        goto cleanup;
    }
    memset(ctx.buf.data + ctx.buf.size, 0, (size_t)index_bytes);
    ctx.buf.size += index_bytes;

    if (srbf_idx_init(&idx_ctx, (uint32_t)(total_nodes > UINT32_MAX ? UINT32_MAX : (uint32_t)total_nodes)) < 0) {
        goto cleanup;
    }
    ctx.idx_ctx = &idx_ctx;

    /* Serialize all top-level siblings (node records start after header + index) */
    for (node = ctx.root; node; node = node->next) {
        uint64_t node_offset = srbf_serialize_node(&ctx, node);

        if (node_offset == 0) {
            fprintf(stderr, "  Error: srbf_serialize_node returned 0 for node %d\n", ncnt);
            goto cleanup;
        }
        if (first_offset == 0) {
            first_offset = node_offset;
            if (first_offset != idx_offset + index_bytes) {
                fprintf(stderr, "  Error: SRBF root offset mismatch (expected index then nodes)\n");
                goto cleanup;
            }
        }
        ncnt++;
    }

    ctx.idx_ctx = NULL;

    if ((uint64_t)idx_ctx.count != total_nodes) {
        fprintf(stderr, "  Error: SRBF index entry count mismatch (%" PRIu32 " vs %" PRIu64 ")\n",
                idx_ctx.count, total_nodes);
        goto cleanup;
    }

    srbf_idx_sort(&idx_ctx);
    memcpy(ctx.buf.data + idx_offset, idx_ctx.entries, idx_ctx.count * sizeof(struct srbf_idx_entry));
    srbf_idx_cleanup(&idx_ctx);
    memset(&idx_ctx, 0, sizeof(idx_ctx));

    if (first_offset == 0) {
        fprintf(stderr, "  Error: No nodes serialized for this module\n");
        goto cleanup;
    }

    /* Build string table */
    strtbl_start = ctx.buf.size;

    /* Write string table entries in insertion order */
    for (struct srbf_strtbl_entry *entry = ctx.strtbl_head; entry; entry = entry->order_next) {
        uint32_t len = strlen(entry->string);

        srbf_buf_write(&ctx.buf, &len, sizeof(len));
        srbf_buf_write(&ctx.buf, entry->string, len);
    }

    /* Fill in header */
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, SRBF_MAGIC, 4);
    hdr.version = SRBF_VERSION;
    hdr.flags = SRBF_FLAG_HAS_STRTBL | SRBF_FLAG_HAS_INDEX;
    hdr.root_offset = first_offset;
    hdr.strtbl_offset = (ctx.strtbl_count > 0) ? strtbl_start : 0;
    hdr.idx_offset = idx_offset;
    hdr.node_count = ctx.node_count;
    hdr.timestamp = time(NULL);

    /* Calculate schema hash */
    srbf_calc_schema_hash(mod, hdr.schema_hash);

    /* Write header at beginning */
    memcpy(ctx.buf.data, &hdr, sizeof(hdr));

    /* Write to file */
    if (write(fd, ctx.buf.data, ctx.buf.size) != (ssize_t)ctx.buf.size) {
        goto cleanup;
    }

    ret = 0;

cleanup:
    srbf_idx_cleanup(&idx_ctx);
    srbf_serctx_cleanup(&ctx);
    return ret;
}

/**
 * @brief Serialize data tree for multiple modules
 *
 * This function handles multi-module data by splitting the tree and
 * storing each module's data in a separate file.
 *
 * @param[in] root Root node of data tree (may contain multiple modules)
 * @param[in] ly_ctx libyang context
 * @param[in] ds Datastore type
 * @return Number of modules written, or -1 on error
 */
API int
srbf_serialize_tree_multi(const struct lyd_node *root, struct ly_ctx *ly_ctx, sr_datastore_t ds)
{
    struct srbf_mod_split *split = NULL;
    char **mod_names = NULL;
    int mod_count = 0;
    int written = 0;
    int ret = -1;

    assert(root);
    assert(ly_ctx);

    /* Split data by module */
    split = srbf_split_by_module((struct lyd_node *)root, ly_ctx);
    if (!split) {
        fprintf(stderr, "Error: Failed to split data by module\n");
        goto cleanup;
    }
    mod_count = srbf_split_get_module_names(split, &mod_names);
    if (mod_count < 0) {
        goto cleanup;
    }

    for (int i = 0; i < mod_count; i++) {
        const char *mod_name = mod_names[i];
        struct lyd_node *mod_data = NULL;
        char *path = NULL;
        int fd = -1;
        const struct lys_module *mod;

        /* Get module data for this module */
        mod_data = srbf_split_get_module_data(split, mod_name);
        if (!mod_data) {
            /* No data for this module */
            fprintf(stderr, "  No data for module '%s'\n", mod_name);
            continue;
        }

        /* Get module from context */
        mod = ly_ctx_get_module_implemented(ly_ctx, mod_name);
        if (!mod) {
            /* Module not implemented in context, skip */
            fprintf(stderr, "  Module '%s' not implemented in context\n", mod_name);
            continue;
        }

        /* Get file path for this module */
        if (srbf_get_path(mod_name, ds, &path) < 0) {
            fprintf(stderr, "  Failed to get path for module '%s'\n", mod_name);
            continue;
        }

        /* Open file for writing */
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "  Failed to open file '%s' for module '%s'\n", path, mod_name);
            free(path);
            continue;
        }

        /* Serialize this module's data */
        if (srbf_serialize_tree(mod_data, fd, mod) == 0) {
            written++;
        } else {
            fprintf(stderr, "  Failed to serialize module '%s'\n", mod_name);
        }

        /* Cleanup */
        close(fd);
        free(path);
    }

    ret = written;

cleanup:
    /* Free module names */
    if (mod_names) {
        for (int i = 0; i < mod_count; i++) {
            free(mod_names[i]);
        }
        free(mod_names);
    }

    /* Free split structure */
    if (split) {
        srbf_free_module_split(split);
    }

    return ret;
}

/**
 * @brief Migrate existing JSON file to binary format
 *
 * Reads JSON file, converts to binary, keeps JSON as backup.
 */
API int
srbf_migrate_keep_json(const char *json_path, const struct lyd_node *mod_data,
                        const struct lys_module *mod)
{
    char *bin_path = NULL;
    int fd = -1, ret = -1;

    assert(json_path);
    assert(mod_data);
    assert(mod);

    /* Generate binary file path */
    if (asprintf(&bin_path, "%s%s", json_path, SRBF_FILE_EXT) < 0) {
        return -1;
    }

    /* Open binary file for writing */
    fd = open(bin_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        goto cleanup;
    }

    /* Serialize to binary format */
    if (srbf_serialize_tree(mod_data, fd, mod) < 0) {
        goto cleanup;
    }

    ret = 0;

cleanup:
    if (fd >= 0) {
        close(fd);
    }
    free(bin_path);
    return ret;
}
