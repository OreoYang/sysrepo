/**
 * @file srbin_deserialize.c
 * @brief SRBF deserialization (read path) - Converts binary format to libyang data tree
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
#include "srbin_deserialize.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libyang/libyang.h>

#include "common.h"

#define srpds_name "SRBF"

#define SRBF_LIST_KEYS_MAX 16

/**
 * @brief Context for deserialization
 */
struct srbf_dectx {
    int fd;                     /**< File descriptor */
    uint8_t *map;               /**< Memory-mapped file data */
    size_t map_size;            /**< Size of memory mapping */
    struct ly_ctx *ly_ctx;      /**< libyang context */
    const struct lys_module *module; /**< YANG module */
    char *strtbl;               /**< String table data */
    struct srbf_idx_entry *idx; /**< Index table */
    uint32_t idx_count;         /**< Number of index entries */
    struct srbf_header hdr;     /**< File header */
};

/**
 * @brief Memory-map the SRBF file
 */
static int
srbf_mmap_file(struct srbf_dectx *ctx, off_t offset, size_t length)
{
    (void)offset;
    (void)length;

    assert(ctx);
    assert(ctx->fd >= 0);

    if (ctx->map) {
        /* Already mapped */
        return 0;
    }

    /* Get file size */
    struct stat st;
    if (fstat(ctx->fd, &st) < 0) {
        return -1;
    }

    ctx->map_size = st.st_size;

    /* Memory-map the file */
    ctx->map = mmap(NULL, ctx->map_size, PROT_READ, MAP_PRIVATE, ctx->fd, 0);
    if (ctx->map == MAP_FAILED) {
        ctx->map = NULL;
        return -1;
    }

    return 0;
}

/**
 * @brief Unmap the SRBF file
 */
static void
srbf_munmap_file(struct srbf_dectx *ctx)
{
    if (ctx && ctx->map) {
        munmap(ctx->map, ctx->map_size);
        ctx->map = NULL;
        ctx->map_size = 0;
    }
}

/**
 * @brief Read a string from the string table into a buffer
 *
 * String table format:
 *   [uint32_t length][char data...][uint32_t length][char data...]...
 *
 * The offset is relative to the string table start.
 */
static void
srbf_read_string_buf(struct srbf_dectx *ctx, uint64_t offset, char *buf, size_t buf_size)
{
    uint32_t len;
    size_t st_len;

    if (!ctx->strtbl || !buf || buf_size == 0) {
        if (buf && buf_size > 0) {
            buf[0] = '\0';
        }
        return;
    }

    st_len = ctx->map_size - ctx->hdr.strtbl_offset;
    if (offset >= st_len || offset + sizeof(len) > st_len) {
        buf[0] = '\0';
        return;
    }

    memcpy(&len, ctx->strtbl + offset, sizeof(len));
    if (offset + sizeof(len) + (size_t)len > st_len) {
        buf[0] = '\0';
        return;
    }

    if (len >= buf_size) {
        len = buf_size - 1;
    }
    if (len > 0) {
        memcpy(buf, ctx->strtbl + offset + sizeof(len), len);
    }
    buf[len] = '\0';
}

static LY_ERR srbf_deserialize_list(struct srbf_dectx *ctx, struct srbf_node *bin_node, struct lyd_node *parent,
        const struct lys_module *node_mod, const char *list_name, struct lyd_node **ly_node_out);

/**
 * @brief Deserialize a single node
 *
 * This is the core deserialization function that converts a binary node
 * back into a libyang lyd_node structure. Handles both regular and opaque nodes.
 */
static struct lyd_node *
srbf_deserialize_node(struct srbf_dectx *ctx, uint64_t offset, struct lyd_node *parent)
{
    struct srbf_node *bin_node;
    struct lyd_node *ly_node = NULL;
    struct lyd_node *tree_first;
    struct lyd_node *child;
    struct lyd_node *sibling;
    LY_ERR ly_err = LY_SUCCESS;
    int is_opaque;
    char node_name_buf[256];
    char mod_name_buf[256];
    char value_buf[512];
    const char *node_name;
    const struct lys_module *node_mod;

    if (offset == 0) {
        return NULL;
    }

    if (offset + sizeof(struct srbf_node) > ctx->map_size) {
        return NULL;
    }

    bin_node = (struct srbf_node *)(ctx->map + offset);

    is_opaque = (bin_node->node_type == LYS_UNKNOWN);

    node_name_buf[0] = '\0';
    mod_name_buf[0] = '\0';
    value_buf[0] = '\0';

    if (is_opaque) {
        srbf_read_string_buf(ctx, bin_node->node_name_off, node_name_buf, sizeof(node_name_buf));
        node_name = node_name_buf;
        srbf_read_string_buf(ctx, bin_node->value_offset, value_buf, sizeof(value_buf));
        ly_err = lyd_new_path(parent, ctx->ly_ctx, node_name,
                value_buf[0] ? value_buf : "", LYD_NEW_PATH_OPAQ, &ly_node);
    } else {
        node_mod = ctx->module;
        if (bin_node->mod_name_off) {
            srbf_read_string_buf(ctx, bin_node->mod_name_off, mod_name_buf, sizeof(mod_name_buf));
            if (mod_name_buf[0]) {
                node_mod = ly_ctx_get_module_implemented(ctx->ly_ctx, mod_name_buf);
                if (!node_mod) {
                    node_mod = ctx->module;
                }
            }
        }

        srbf_read_string_buf(ctx, bin_node->node_name_off, node_name_buf, sizeof(node_name_buf));
        node_name = node_name_buf;
        if (!node_name[0]) {
            return NULL;
        }

        srbf_read_string_buf(ctx, bin_node->value_offset, value_buf, sizeof(value_buf));

        switch (bin_node->node_type) {
        case LYS_LEAF:
        case LYS_LEAFLIST:
            ly_err = lyd_new_term(parent, node_mod, node_name,
                    value_buf[0] ? value_buf : NULL, 0, &ly_node);
            break;
        case LYS_LIST:
            ly_err = srbf_deserialize_list(ctx, bin_node, parent, node_mod, node_name, &ly_node);
            break;
        case LYS_CONTAINER:
        case LYS_CASE:
        case LYS_NOTIF:
        case LYS_RPC:
        case LYS_ACTION:
            ly_err = lyd_new_inner(parent, node_mod, node_name, 0, &ly_node);
            break;
        default:
            ly_err = lyd_new_inner(parent, node_mod, node_name, 0, &ly_node);
            break;
        }
    }

    if (ly_err != LY_SUCCESS || !ly_node) {
        return NULL;
    }

    tree_first = ly_node;

    /*
     * All children: walk first_child and its next_sibling chain.
     * Use only lyd_insert_child(): libyang places nodes per schema order.
     * lyd_insert_after() is illegal for system-ordered siblings (see libyang tree_data.h).
     * Lists attach children inside srbf_deserialize_list().
     */
    if (bin_node->node_type != LYS_LIST && bin_node->first_child != 0) {
        uint64_t ch_off = bin_node->first_child;

        while (ch_off != 0) {
            struct srbf_node *ch_bin;

            if (ch_off + sizeof(struct srbf_node) > ctx->map_size) {
                break;
            }

            child = srbf_deserialize_node(ctx, ch_off, ly_node);
            if (!child) {
                break;
            }

            if (lyd_insert_child(ly_node, child) != LY_SUCCESS) {
                lyd_free_tree(child);
                break;
            }

            ch_bin = (struct srbf_node *)(ctx->map + ch_off);
            ch_off = ch_bin->next_sibling;
        }
    }

    /*
     * Top-level siblings only (parent == NULL in this recursion).
     * Non-root siblings under one parent are linked only via that parent's
     * first_child / next_sibling walk above — handling next_sibling here too
     * would deserialize and insert the same node twice.
     */
    if (bin_node->next_sibling != 0 && parent == NULL) {
        uint64_t sib_off = bin_node->next_sibling;
        struct lyd_node *anchor = ly_node;

        while (sib_off != 0) {
            struct srbf_node *sib_bin;

            if (sib_off + sizeof(struct srbf_node) > ctx->map_size) {
                break;
            }

            sibling = srbf_deserialize_node(ctx, sib_off, NULL);
            if (!sibling) {
                break;
            }

            if (lyd_insert_sibling(anchor, sibling, &tree_first) != LY_SUCCESS) {
                lyd_free_tree(sibling);
                break;
            }
            anchor = sibling;

            sib_bin = (struct srbf_node *)(ctx->map + sib_off);
            sib_off = sib_bin->next_sibling;
        }
    }

    return parent == NULL ? tree_first : ly_node;
}

/**
 * @brief Create a list instance from binary children (key leaves + payload).
 */
static LY_ERR
srbf_deserialize_list(struct srbf_dectx *ctx, struct srbf_node *bin_node, struct lyd_node *parent,
        const struct lys_module *node_mod, const char *list_name, struct lyd_node **ly_node_out)
{
    const struct lysc_node *par_sc = parent ? parent->schema : NULL;
    const struct lysc_node *lsc;
    const struct lysc_node *kn;
    const char *key_vals[SRBF_LIST_KEYS_MAX];
    char key_storage[SRBF_LIST_KEYS_MAX][256];
    const struct lysc_node *kschema[SRBF_LIST_KEYS_MAX];
    unsigned int nk = 0, ki;
    LY_ERR ly_err = LY_SUCCESS;
    uint64_t co;

    *ly_node_out = NULL;

    lsc = lys_find_child(ctx->ly_ctx, par_sc, node_mod, NULL, 0, list_name, 0, 0);
    if (!lsc || (lsc->nodetype != LYS_LIST)) {
        return LY_EINVAL;
    }

    if (lsc->flags & LYS_KEYLESS) {
        ly_err = lyd_new_list2(parent, node_mod, list_name, NULL, 0, ly_node_out);
        if (ly_err != LY_SUCCESS) {
            return ly_err;
        }
    } else {
        for (kn = lysc_node_child(lsc); kn && nk < SRBF_LIST_KEYS_MAX; kn = kn->next) {
            if ((kn->nodetype == LYS_LEAF) && (kn->flags & LYS_KEY)) {
                kschema[nk++] = kn;
            }
        }

        if (nk == 0) {
            return LY_EINVAL;
        }

        for (ki = 0; ki < nk; ki++) {
            key_storage[ki][0] = '\0';
            key_vals[ki] = key_storage[ki];
        }

        for (co = bin_node->first_child; co != 0; ) {
            struct srbf_node *bc;
            char nmbuf[256];

            if (co + sizeof(struct srbf_node) > ctx->map_size) {
                break;
            }

            bc = (struct srbf_node *)(ctx->map + co);
            co = bc->next_sibling;

            if (bc->node_type != LYS_LEAF) {
                continue;
            }

            srbf_read_string_buf(ctx, bc->node_name_off, nmbuf, sizeof(nmbuf));
            for (ki = 0; ki < nk; ki++) {
                if (strcmp(nmbuf, kschema[ki]->name) == 0) {
                    srbf_read_string_buf(ctx, bc->value_offset, key_storage[ki], sizeof(key_storage[ki]));
                    break;
                }
            }
        }

        for (ki = 0; ki < nk; ki++) {
            if (key_storage[ki][0] == '\0') {
                return LY_EINVAL;
            }
        }

        ly_err = lyd_new_list3(parent, node_mod, list_name, (const void **)key_vals, NULL, 0, ly_node_out);
        if (ly_err != LY_SUCCESS) {
            return ly_err;
        }
    }

    for (co = bin_node->first_child; co != 0; ) {
        struct srbf_node *bc;
        char nmbuf[256];
        struct lyd_node *ch;
        const struct lysc_node *ch_sc;

        if (co + sizeof(struct srbf_node) > ctx->map_size) {
            break;
        }

        bc = (struct srbf_node *)(ctx->map + co);
        co = bc->next_sibling;

        if (bc->node_type == LYS_LEAF) {
            srbf_read_string_buf(ctx, bc->node_name_off, nmbuf, sizeof(nmbuf));
            ch_sc = lys_find_child(ctx->ly_ctx, lsc, node_mod, NULL, 0, nmbuf, 0, 0);
            if (ch_sc && (ch_sc->nodetype == LYS_LEAF) && (ch_sc->flags & LYS_KEY)) {
                continue;
            }
        }

        ch = srbf_deserialize_node(ctx, (uint64_t)((uint8_t *)bc - ctx->map), *ly_node_out);
        if (!ch) {
            continue;
        }

        if (lyd_insert_child(*ly_node_out, ch) != LY_SUCCESS) {
            lyd_free_tree(ch);
            lyd_free_tree(*ly_node_out);
            *ly_node_out = NULL;
            return LY_EINT;
        }
    }

    return LY_SUCCESS;
}

/**
 * @brief Initialize deserialization context
 */
static int
srbf_dectx_init(struct srbf_dectx *ctx, int fd, struct ly_ctx *ly_ctx, const struct lys_module *mod)
{
    assert(ctx);
    assert(fd >= 0);
    assert(ly_ctx);
    assert(mod);

    memset(ctx, 0, sizeof(*ctx));

    ctx->fd = fd;
    ctx->ly_ctx = ly_ctx;
    ctx->module = mod;

    if (lseek(fd, 0, SEEK_SET) < 0) {
        return -1;
    }

    /* Read header */
    if (srbf_read_header(fd, &ctx->hdr) < 0) {
        return -1;
    }

    /* Validate header */
    if (srbf_validate_header(&ctx->hdr, mod) == 0) {
        /* Temporarily skip validation to debug */
        /* return -1; */
    }

    /* Memory-map the file */
    if (srbf_mmap_file(ctx, 0, 0) < 0) {
        return -1;
    }

    /* Setup string table if present */
    if (ctx->hdr.flags & SRBF_FLAG_HAS_STRTBL) {
        ctx->strtbl = (char *)(ctx->map + ctx->hdr.strtbl_offset);
    }

    /* Setup index table if present (entries occupy [idx_offset, root_offset)) */
    if (ctx->hdr.flags & SRBF_FLAG_HAS_INDEX) {
        if (ctx->hdr.root_offset > ctx->hdr.idx_offset) {
            ctx->idx = (struct srbf_idx_entry *)(ctx->map + ctx->hdr.idx_offset);
            ctx->idx_count = (uint32_t)((ctx->hdr.root_offset - ctx->hdr.idx_offset) / sizeof(struct srbf_idx_entry));
        } else {
            ctx->idx = NULL;
            ctx->idx_count = 0;
        }
    }

    return 0;
}

/**
 * @brief Clean up deserialization context
 */
static void
srbf_dectx_cleanup(struct srbf_dectx *ctx)
{
    if (ctx) {
        srbf_munmap_file(ctx);
        ctx->fd = -1;
        ctx->strtbl = NULL;
        ctx->idx = NULL;
        ctx->idx_count = 0;
    }
}

/**
 * @brief Deserialize entire tree from binary format
 *
 * This is the main entry point for deserialization.
 */
API int
srbf_deserialize_tree(int fd, struct ly_ctx *ly_ctx, const struct lys_module *mod, struct lyd_node **root)
{
    struct srbf_dectx ctx;
    int ret = -1;

    assert(fd >= 0);
    assert(ly_ctx);
    assert(mod);
    assert(root);

    *root = NULL;

    /* Initialize deserialization context (reads and stores header) */
    if (srbf_dectx_init(&ctx, fd, ly_ctx, mod) < 0) {
        ret = -1;
        goto cleanup;
    }

    /* Deserialize root node */
    *root = srbf_deserialize_node(&ctx, ctx.hdr.root_offset, NULL);
    if (*root) {
        ret = 0;
    }

cleanup:
    srbf_dectx_cleanup(&ctx);
    return ret;
}

/**
 * @brief Deserialize a subtree from binary format
 *
 * This function deserializes only a portion of the tree, starting from
 * a specific offset. Useful for partial loading.
 */
API int
srbf_deserialize_subtree(int fd, uint64_t offset, struct ly_ctx *ly_ctx,
                          const struct lys_module *mod, struct lyd_node **subtree)
{
    struct srbf_dectx ctx;
    int ret = -1;

    assert(fd >= 0);
    assert(ly_ctx);
    assert(mod);
    assert(subtree);

    *subtree = NULL;

    /* Initialize deserialization context */
    if (srbf_dectx_init(&ctx, fd, ly_ctx, mod) < 0) {
        return -1;
    }

    /* Deserialize node at offset */
    *subtree = srbf_deserialize_node(&ctx, offset, NULL);
    if (*subtree) {
        ret = 0;
    }

    srbf_dectx_cleanup(&ctx);
    return ret;
}

/**
 * @brief Deserialize multi-module data from binary format
 *
 * This function loads data from multiple .srbf files (one per module)
 * and merges them into a single data tree.
 *
 * @param[in] ly_ctx libyang context
 * @param[in] ds Datastore type
 * @param[out] root Merged data tree
 * @return Number of modules loaded, or -1 on error
 */
API int
srbf_deserialize_tree_multi(struct ly_ctx *ly_ctx, sr_datastore_t ds, struct lyd_node **root)
{
    struct lyd_node *merged_tree = NULL;
    uint32_t idx = 0;
    int loaded = 0;
    const struct lys_module *mod;

    assert(ly_ctx);
    assert(root);

    *root = NULL;

    /* Iterate through all implemented modules in the context */
    while ((mod = ly_ctx_get_module_iter(ly_ctx, &idx))) {
        char *path = NULL;
        int fd = -1;
        struct lyd_node *mod_data = NULL;

        /* Only process implemented modules */
        if (!mod->implemented) {
            continue;
        }

        /* Get file path for this module */
        if (srbf_get_path(mod->name, ds, &path) < 0) {
            continue;
        }

        /* Check if file exists */
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            /* File doesn't exist, skip this module */
            free(path);
            continue;
        }

        /* Check if it's a valid SRBF file */
        if (!srbf_is_binary_file(fd)) {
            close(fd);
            free(path);
            continue;
        }

        /* Deserialize this module's data */
        if (srbf_deserialize_tree(fd, ly_ctx, mod, &mod_data) == 0) {
            if (mod_data) {
                /* Merge with existing tree */
                if (merged_tree == NULL) {
                    merged_tree = mod_data;
                    loaded++;
                } else {
                    if (lyd_merge_siblings(&merged_tree, mod_data, LYD_MERGE_DESTRUCT) != LY_SUCCESS) {
                        fprintf(stderr, "  Failed to merge module '%s'\n", mod->name);
                        lyd_free_all(mod_data);
                    } else {
                        loaded++;
                    }
                }
            } else {
                fprintf(stderr, "  Deserialization returned success but no data for '%s'\n", mod->name);
            }
        } else {
            fprintf(stderr, "  Failed to deserialize module '%s'\n", mod->name);
        }

        close(fd);
        free(path);
    }

    if (loaded == 0) {
        fprintf(stderr, "Debug: No modules loaded from SRBF files\n");
    }

    *root = merged_tree;
    return loaded;
}

/**
 * @brief Look up a node by XPath hash using the index table
 */
API int
srbf_lookup_by_hash(int fd, uint64_t hash, struct ly_ctx *ly_ctx,
                    const struct lys_module *mod, struct lyd_node **result)
{
    struct srbf_dectx ctx;
    uint64_t offset = 0;

    assert(fd >= 0);
    assert(ly_ctx);
    assert(mod);
    assert(result);

    *result = NULL;

    /* Initialize context to access index */
    if (srbf_dectx_init(&ctx, fd, ly_ctx, mod) < 0) {
        return -1;
    }

    if (!(ctx.hdr.flags & SRBF_FLAG_HAS_INDEX)) {
        /* No index available */
        goto cleanup;
    }

    /* Linear scan (index is sorted by hash; could use srbf_idx_bsearch) */
    for (uint32_t i = 0; i < ctx.idx_count; i++) {
        if (ctx.idx[i].path_hash == hash) {
            offset = ctx.idx[i].node_offset;
            break;
        }
    }

    if (offset != 0) {
        *result = srbf_deserialize_node(&ctx, offset, NULL);
    }

cleanup:
    srbf_dectx_cleanup(&ctx);
    return (*result != NULL) ? 0 : -1;
}
