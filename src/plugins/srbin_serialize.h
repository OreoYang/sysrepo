/**
 * @file srbin_serialize.h
 * @brief SRBF serialization public API
 *
 * @copyright
 * Copyright (c) 2026 Vecima Networks Inc.
 *
 * This source code is licensed under BSD-3-Clause License (the "License").
 */

#ifndef _SRBIN_SERIALIZE_H
#define _SRBIN_SERIALIZE_H

#include <stdint.h>

#include <libyang/libyang.h>

#include "config.h"
#include "sysrepo.h"

/**
 * @brief Serialize data tree to binary format
 *
 * @param[in] root Data tree to serialize
 * @param[in] fd File descriptor to write to
 * @param[in] mod YANG module
 * @return 0 on success, -1 on error
 */
API int srbf_serialize_tree(const struct lyd_node *root, int fd, const struct lys_module *mod);

/**
 * @brief Serialize multi-module data tree to binary format
 *
 * Splits the data tree by module and stores each module's data
 * in a separate .srbf file.
 *
 * @param[in] root Data tree to serialize (may contain multiple modules)
 * @param[in] ly_ctx libyang context
 * @param[in] ds Datastore type
 * @return Number of modules written, or -1 on error
 */
API int srbf_serialize_tree_multi(const struct lyd_node *root, struct ly_ctx *ly_ctx, sr_datastore_t ds);

/**
 * @brief Migrate existing JSON file to binary format
 *
 * Reads JSON file, converts to binary, keeps JSON as backup.
 *
 * @param[in] json_path Path to JSON file
 * @param[in] mod_data Data to serialize
 * @param[in] mod YANG module
 * @return 0 on success, -1 on error
 */
API int srbf_migrate_keep_json(const char *json_path, const struct lyd_node *mod_data,
                            const struct lys_module *mod);

#endif /* _SRBIN_SERIALIZE_H */
