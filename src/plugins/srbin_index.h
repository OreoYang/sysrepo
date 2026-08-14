/**
 * @file srbin_index.h
 * @brief SRBF XPath index table for O(1) path lookups
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

#ifndef _SRBIN_INDEX_H
#define _SRBIN_INDEX_H

#include "config.h"

#include <stdint.h>

#include <libyang/libyang.h>

#include "common_srbin.h"

/**
 * @brief Index building context
 */
struct srbf_idx_ctx {
    struct srbf_idx_entry *entries;
    uint32_t count;
    uint32_t capacity;
};

/**
 * @brief Calculate FNV-1a hash of an XPath
 *
 * @param[in] xpath XPath string to hash
 * @return 64-bit hash value
 */
API uint64_t srbf_xpath_hash(const char *xpath);

/**
 * @brief Initialize index building context
 *
 * @param[out] ctx Context to initialize
 * @param[in] capacity Initial capacity
 * @return 0 on success, -1 on error
 */
API int srbf_idx_init(struct srbf_idx_ctx *ctx, uint32_t capacity);

/**
 * @brief Clean up index context
 *
 * @param[in] ctx Context to clean up
 */
API void srbf_idx_cleanup(struct srbf_idx_ctx *ctx);

/**
 * @brief Add entry to index
 *
 * @param[in] ctx Index context
 * @param[in] xpath XPath string
 * @param[in] offset Node offset in file
 * @return 0 on success, -1 on error
 */
API int srbf_idx_add(struct srbf_idx_ctx *ctx, const char *xpath, uint64_t offset);

/**
 * @brief Sort index entries by hash
 *
 * @param[in] ctx Index context
 */
API void srbf_idx_sort(struct srbf_idx_ctx *ctx);

/**
 * @brief Write index table to file
 *
 * @param[in] fd File descriptor
 * @param[in] ctx Index context
 * @return 0 on success, -1 on error
 */
API int srbf_idx_write(int fd, const struct srbf_idx_ctx *ctx);

/**
 * @brief Walk a data tree and build an in-memory XPath index (offsets often 0 until matched to serialization)
 */
API int srbf_build_index(struct lyd_node *root, struct srbf_idx_ctx *ctx);

/**
 * @brief Load subtree by XPath using index
 *
 * @param[in] fd File descriptor
 * @param[in] xpath XPath to lookup
 * @param[in] ly_ctx libyang context
 * @param[in] mod YANG module
 * @param[out] result Deserialized subtree
 * @return 0 on success, -1 on error
 */
API int srbf_load_subtree_by_xpath(int fd, const char *xpath, struct ly_ctx *ly_ctx,
                                const struct lys_module *mod, struct lyd_node **result);

#endif /* _SRBIN_INDEX_H */
