/**
 * @file srbin_deserialize.h
 * @brief SRBF deserialization public API
 *
 * @copyright
 * Copyright (c) 2026 Vecima Networks Inc.
 *
 * This source code is licensed under BSD-3-Clause License (the "License").
 */

#ifndef _SRBIN_DESERIALIZE_H
#define _SRBIN_DESERIALIZE_H

#include <stdint.h>

#include <libyang/libyang.h>

#include "common_srbin.h"
#include "sysrepo.h"

/**
 * @brief Deserialize entire tree from binary format
 *
 * @param[in] fd File descriptor
 * @param[in] ly_ctx libyang context
 * @param[in] mod YANG module
 * @param[out] root Deserialized data tree
 * @return 0 on success, -1 on error
 */
API int srbf_deserialize_tree(int fd, struct ly_ctx *ly_ctx, const struct lys_module *mod, struct lyd_node **root);

/**
 * @brief Deserialize multi-module data from binary format
 *
 * Loads data from multiple .srbf files (one per module) and merges them.
 *
 * @param[in] ly_ctx libyang context
 * @param[in] ds Datastore type
 * @param[out] root Merged data tree
 * @return Number of modules loaded, or -1 on error
 */
API int srbf_deserialize_tree_multi(struct ly_ctx *ly_ctx, sr_datastore_t ds, struct lyd_node **root);

/**
 * @brief Deserialize a subtree from binary format
 *
 * @param[in] fd File descriptor
 * @param[in] offset Node offset in file
 * @param[in] ly_ctx libyang context
 * @param[in] mod YANG module
 * @param[out] subtree Deserialized subtree
 * @return 0 on success, -1 on error
 */
API int srbf_deserialize_subtree(int fd, uint64_t offset, struct ly_ctx *ly_ctx,
                              const struct lys_module *mod, struct lyd_node **subtree);

/**
 * @brief Look up a node by XPath hash using the index table
 *
 * @param[in] fd File descriptor
 * @param[in] hash XPath hash
 * @param[in] ly_ctx libyang context
 * @param[in] mod YANG module
 * @param[out] result Deserialized node
 * @return 0 on success, -1 on error
 */
API int srbf_lookup_by_hash(int fd, uint64_t hash, struct ly_ctx *ly_ctx,
                        const struct lys_module *mod, struct lyd_node **result);

#endif /* _SRBIN_DESERIALIZE_H */
