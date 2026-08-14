/**
 * @file common_srbin.h
 * @brief Sysrepo Binary Format (SRBF) - PostgreSQL jsonb-inspired binary storage
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

#ifndef _COMMON_SRBIN_H
#define _COMMON_SRBIN_H

#include "config.h"

#include <stdint.h>
#include <sys/types.h>

#include <libyang/libyang.h>

#include "sysrepo.h"

/**
 * @brief SRBF magic bytes
 */
#define SRBF_MAGIC "SRBF"

/**
 * @brief Current SRBF format version
 *
 * v3: struct srbf_node — uint16_t node_type (full LYS_* flags), mod_name_off,
 *     node_name_off, string-table references (v2 packed schema_id removed).
 */
#define SRBF_VERSION 3

/**
 * @brief SRBF file extension
 */
#define SRBF_FILE_EXT ".srbf"

/**
 * @brief SRBF flags
 */
enum srbf_flag_e {
    SRBF_FLAG_NONE         = 0x00000000, /**< No flags */
    SRBF_FLAG_COMPRESSED   = 0x00000001, /**< Data is compressed */
    SRBF_FLAG_HAS_INDEX    = 0x00000002, /**< Index table is present */
    SRBF_FLAG_HAS_STRTBL   = 0x00000004, /**< String table is present */
};

/**
 * @brief SRBF header (on-disk format, 64 bytes, must be packed)
 *
 * The header is stored at the beginning of every .srbf file.
 * All multi-byte integers are stored in little-endian format.
 */
struct srbf_header {
    char magic[4];              /**< Magic bytes "SRBF" */
    uint32_t version;           /**< Format version */
    uint32_t flags;             /**< Format flags (enum srbf_flag_e) */
    uint8_t schema_hash[32];    /**< SHA256 hash of module schema */
    uint64_t root_offset;       /**< Offset to first node record from file start (after index when HAS_INDEX) */
    uint64_t strtbl_offset;     /**< Offset to string table from file start */
    uint64_t idx_offset;        /**< Offset to index table from file start (0 if no index) */
    uint64_t node_count;        /**< Total number of nodes in tree */
    uint64_t timestamp;         /**< Modification time (seconds since epoch) */
    uint64_t reserved;          /**< Reserved for future use */
} __attribute__((packed));

/**
 * @brief SRBF node encoding (on-disk format)
 *
 * Each node in the data tree is encoded as this structure.
 * Offsets are relative to the beginning of the file.
 */
struct srbf_node {
    uint16_t node_type;         /**< libyang schema nodetype (LYS_*), 16-bit flags */
    uint16_t value_type;        /**< Value type (enum sr_type_t or LY_TYPE_*) */
    uint8_t dflt_flag;          /**< 1 if this is a default value, 0 otherwise */
    uint8_t reserved;          /**< Padding, set to 0 */
    /**
     * String-table offset (relative to string table start) of owning module name.
     * 0 means the module for this .srbf file (deserialize ctx->module).
     */
    uint32_t mod_name_off;
    /** String-table offset of schema local name (opaque: node name). */
    uint32_t node_name_off;
    uint64_t value_offset;      /**< String-table offset of value (0 if none) */
    uint64_t first_child;       /**< File offset to first child node (0 if none) */
    uint64_t next_sibling;      /**< File offset to next sibling node (0 if none) */
    uint64_t parent;            /**< File offset to parent node (0 if root) */
} __attribute__((packed));

/**
 * @brief SRBF index entry (for XPath lookups)
 */
struct srbf_idx_entry {
    uint64_t path_hash;         /**< FNV-1a hash of XPath */
    uint64_t node_offset;       /**< Offset to node in file */
} __attribute__((packed));

/**
 * @brief String table entry
 */
struct srbf_str_entry {
    uint32_t length;            /**< String length (not including null terminator) */
    char string[];              /**< String data (not null-terminated in storage) */
} __attribute__((packed));

/**
 * @brief Serialization buffer
 */
struct srbf_buf {
    uint8_t *data;              /**< Buffer data */
    uint64_t size;              /**< Current size */
    uint64_t capacity;          /**< Allocated capacity */
    uint64_t root_offset;       /**< Offset to root node */
};

/**
 * @brief Check if a file is in SRBF format
 *
 * @param[in] fd Open file descriptor (must be positioned at beginning)
 * @return 1 if SRBF format, 0 otherwise
 */
API int srbf_is_binary_file(int fd);

/**
 * @brief Write SRBF header to file
 *
 * @param[in] fd Open file descriptor
 * @param[in] hdr Header to write
 * @return 0 on success, -1 on error
 */
API int srbf_write_header(int fd, const struct srbf_header *hdr);

/**
 * @brief Read SRBF header from file
 *
 * @param[in] fd Open file descriptor
 * @param[out] hdr Header to read into
 * @return 0 on success, -1 on error
 */
API int srbf_read_header(int fd, struct srbf_header *hdr);

/**
 * @brief Validate SRBF header
 *
 * @param[in] hdr Header to validate
 * @param[in] mod Module to compare schema hash against (can be NULL)
 * @return 1 if valid, 0 if invalid
 */
API int srbf_validate_header(const struct srbf_header *hdr, const struct lys_module *mod);

/**
 * @brief Calculate schema hash for a module
 *
 * @param[in] mod Module to calculate hash for
 * @param[out] hash_out 32-byte hash output
 * @return 0 on success, -1 on error
 */
API int srbf_calc_schema_hash(const struct lys_module *mod, uint8_t hash_out[32]);

/**
 * @brief Get SRBF file path for a module
 *
 * @param[in] module_name Module name
 * @param[in] ds Datastore type
 * @param[out] path_out Allocated path (must be freed by caller)
 * @return 0 on success, -1 on error
 */
API int srbf_get_path(const char *module_name, sr_datastore_t ds, char **path_out);

/**
 * @brief Get SRBF file path for operational data
 *
 * @param[in] module_name Module name
 * @param[in] cid Connection ID
 * @param[in] sid Session ID
 * @param[out] path_out Allocated path (must be freed by caller)
 * @return 0 on success, -1 on error
 */
API int srbf_get_oper_path(const char *module_name, sr_cid_t cid, uint32_t sid, char **path_out);

/**
 * @brief Check if SRBF file exists
 *
 * @param[in] module_name Module name
 * @param[in] ds Datastore type
 * @return 1 if exists, 0 otherwise
 */
API int srbf_file_exists(const char *module_name, sr_datastore_t ds);

/**
 * @brief Opaque structure for module split
 */
struct srbf_mod_split;

/**
 * @brief Split data tree by module
 *
 * Separates a multi-module data tree into per-module subtrees.
 *
 * @param[in] root Root node of data tree
 * @param[in] ly_ctx libyang context
 * @return Split structure on success, NULL on error
 */
struct srbf_mod_split *srbf_split_by_module(struct lyd_node *root, struct ly_ctx *ly_ctx);

/**
 * @brief Get module data from split structure
 *
 * @param[in] split Split structure
 * @param[in] mod_name Module name
 * @return Module data tree, or NULL if not found
 */
struct lyd_node *srbf_split_get_module_data(struct srbf_mod_split *split, const char *mod_name);

/**
 * @brief Get all module names from split structure
 *
 * @param[in] split Split structure
 * @param[out] names Array of module names (must be freed by caller)
 * @return Number of modules, or -1 on error
 */
int srbf_split_get_module_names(struct srbf_mod_split *split, char ***names);

/**
 * @brief Free module split structure
 *
 * @param[in] split Split structure to free
 */
void srbf_free_module_split(struct srbf_mod_split *split);

#endif /* _COMMON_SRBIN_H */
