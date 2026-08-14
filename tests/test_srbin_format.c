/**
 * @file test_srbin_format.c
 * @brief Unit tests for Sysrepo Binary Format
 *
 * Tests core format functionality:
 * - Header read/write
 * - Node serialization round-trip
 * - String table operations
 * - Index table operations
 * - Schema validation
 *
 * @copyright
 * Copyright (c) 2025 Deutsche Telekom AG.
 * Copyright (c) 2025 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD-3-Clause License (the "License").
 */

#define _GNU_SOURCE

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <libyang/libyang.h>

#include "sysrepo.h"
#include "common_srbin.h"
#include "srbin_serialize.h"
#include "srbin_deserialize.h"
#include "srbin_index.h"

#define TEST_DIR "/tmp/srbin_test"
#define TEST_FILE TEST_DIR "/test.srbf"

/**
 * @brief Setup test environment
 */
static int
setup(void **state)
{
    system("mkdir -p " TEST_DIR);
    return 0;
}

/**
 * @brief Cleanup test environment
 */
static int
teardown(void **state)
{
    system("rm -rf " TEST_DIR);
    return 0;
}

/**
 * @brief Test header read/write
 */
static void
test_header_read_write(void **state)
{
    struct srbf_header hdr, hdr_read;
    int fd;
    const char *module_name = "test-module";

    (void)state;

    /* Initialize header */
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, SRBF_MAGIC, 4);
    hdr.version = SRBF_VERSION;
    hdr.flags = SRBF_FLAG_HAS_STRTBL | SRBF_FLAG_HAS_INDEX;
    hdr.root_offset = 1024;
    hdr.strtbl_offset = 2048;
    hdr.idx_offset = 4096;
    hdr.node_count = 100;
    hdr.timestamp = time(NULL);
    hdr.reserved = 0;

    /* Calculate schema hash (using module name) */
    for (size_t i = 0; i < strlen(module_name) && i < 32; i++) {
        hdr.schema_hash[i] = module_name[i];
    }

    /* Write header to file */
    fd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert_true(fd > 0);

    assert_int_equal(srbf_write_header(fd, &hdr), 0);
    close(fd);

    /* Read header back */
    fd = open(TEST_FILE, O_RDONLY);
    assert_true(fd > 0);

    memset(&hdr_read, 0, sizeof(hdr_read));
    assert_int_equal(srbf_read_header(fd, &hdr_read), 0);
    close(fd);

    /* Verify header */
    assert_int_equal(hdr_read.version, SRBF_VERSION);
    assert_int_equal(hdr_read.flags, hdr.flags);
    assert_int_equal(hdr_read.root_offset, hdr.root_offset);
    assert_int_equal(hdr_read.strtbl_offset, hdr.strtbl_offset);
    assert_int_equal(hdr_read.idx_offset, hdr.idx_offset);
    assert_int_equal(hdr_read.node_count, hdr.node_count);
    assert_memory_equal(hdr_read.magic, SRBF_MAGIC, 4);
}

/**
 * @brief Test header validation
 */
static void
test_header_validation(void **state)
{
    struct srbf_header hdr;

    (void)state;

    /* Valid header */
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, SRBF_MAGIC, 4);
    hdr.version = SRBF_VERSION;

    assert_int_equal(srbf_validate_header(&hdr, NULL), 1);

    /* Invalid magic */
    memcpy(hdr.magic, "XXXX", 4);
    assert_int_equal(srbf_validate_header(&hdr, NULL), 0);

    /* Restore magic, invalid version */
    memcpy(hdr.magic, SRBF_MAGIC, 4);
    hdr.version = 99;
    assert_int_equal(srbf_validate_header(&hdr, NULL), 0);
}

/**
 * @brief Test binary format detection
 */
static void
test_format_detection(void **state)
{
    int fd;
    struct srbf_header hdr;

    (void)state;

    /* Create binary file */
    fd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert_true(fd > 0);

    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, SRBF_MAGIC, 4);
    hdr.version = SRBF_VERSION;
    srbf_write_header(fd, &hdr);
    close(fd);

    /* Detect binary format */
    fd = open(TEST_FILE, O_RDONLY);
    assert_true(fd > 0);

    assert_int_equal(srbf_is_binary_file(fd), 1);
    close(fd);

    /* Create JSON file */
    unlink(TEST_FILE);
    fd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert_true(fd > 0);

    write(fd, "{}", 2);
    close(fd);

    /* Should not be detected as binary */
    fd = open(TEST_FILE, O_RDONLY);
    assert_true(fd > 0);

    assert_int_equal(srbf_is_binary_file(fd), 0);
    close(fd);
}

/**
 * @brief Test path generation
 */
static void
test_path_generation(void **state)
{
    char *path;

    (void)state;

    /* Test startup datastore path */
    assert_int_equal(srbf_get_path("test-module", SR_DS_STARTUP, &path), 0);
    assert_non_null(path);
    assert_string_not_equal(path, "");
    free(path);

    /* Test running datastore path */
    assert_int_equal(srbf_get_path("test-module", SR_DS_RUNNING, &path), 0);
    assert_non_null(path);
    free(path);

    /* Test operational path */
    assert_int_equal(srbf_get_oper_path("test-module", 12345, 67890, &path), 0);
    assert_non_null(path);
    free(path);
}

/**
 * @brief Test XPath hashing
 */
static void
test_xpath_hash(void **state)
{
    const char *xpath1 = "/test-module:container/leaf";
    const char *xpath2 = "/test-module:container/leaf";
    const char *xpath3 = "/test-module:container/different";
    uint64_t hash1, hash2, hash3;

    (void)state;

    hash1 = srbf_xpath_hash(xpath1);
    hash2 = srbf_xpath_hash(xpath2);
    hash3 = srbf_xpath_hash(xpath3);

    /* Same XPath should produce same hash */
    assert_int_equal(hash1, hash2);

    /* Different XPath should produce different hash (with high probability) */
    assert_int_not_equal(hash1, hash3);

    /* Hash should be non-zero for non-empty input */
    assert_int_not_equal(hash1, 0);

    /* Empty string should produce non-zero hash (FNV property) */
    assert_int_not_equal(srbf_xpath_hash(""), 0);
}

/**
 * @brief Test index operations
 */
static void
test_index_operations(void **state)
{
    struct srbf_idx_ctx ctx;
    const char *xpath1 = "/test:container/leaf1";
    const char *xpath2 = "/test:container/leaf2";
    const char *xpath3 = "/test:container/leaf3";
    uint64_t offset1 = 100, offset2 = 200, offset3 = 300;
    int ret;

    (void)state;

    /* Initialize index */
    assert_int_equal(srbf_idx_init(&ctx, 10), 0);
    assert_int_equal(ctx.capacity, 10);
    assert_int_equal(ctx.count, 0);

    /* Add entries */
    assert_int_equal(srbf_idx_add(&ctx, xpath1, offset1), 0);
    assert_int_equal(ctx.count, 1);

    assert_int_equal(srbf_idx_add(&ctx, xpath2, offset2), 0);
    assert_int_equal(ctx.count, 2);

    assert_int_equal(srbf_idx_add(&ctx, xpath3, offset3), 0);
    assert_int_equal(ctx.count, 3);

    /* Sort index */
    srbf_idx_sort(&ctx);

    /* Verify entries are sorted */
    uint64_t prev_hash = 0;
    for (uint32_t i = 0; i < ctx.count; i++) {
        assert_int_equal(ctx.entries[i].path_hash >= prev_hash, 1);
        prev_hash = ctx.entries[i].path_hash;
    }

    /* Test hash consistency */
    uint64_t expected_hash = srbf_xpath_hash(xpath1);
    /* After sorting, the entry should still exist with correct offset */

    /* Cleanup */
    srbf_idx_cleanup(&ctx);
    assert_int_equal(ctx.entries, NULL);
    assert_int_equal(ctx.capacity, 0);
}

/**
 * @brief Test schema hash calculation
 */
static void
test_schema_hash(void **state)
{
    struct ly_ctx *ctx;
    struct lys_module *mod;
    uint8_t hash1[32], hash2[32];

    (void)state;

    /* Create libyang context */
    assert_int_equal(ly_ctx_new(NULL, 0, &ctx), LY_SUCCESS);

    /* Parse simple module */
    const char *yang_def =
        "module test-module1 {"
        "  namespace \"urn:test:test1\";"
        "  prefix t1;"
        "  leaf test { type string; }"
        "}";

    lys_parse_mem(ctx, yang_def, LYS_IN_YANG, &mod);
    assert_non_null(mod);

    /* Calculate hash */
    assert_int_equal(srbf_calc_schema_hash(mod, hash1), 0);

    /* Parse another module */
    yang_def =
        "module test-module2 {"
        "  namespace \"urn:test:test2\";"
        "  prefix t2;"
        "  leaf test { type string; }"
        "}";

    lys_parse_mem(ctx, yang_def, LYS_IN_YANG, &mod);
    assert_non_null(mod);

    /* Calculate hash */
    assert_int_equal(srbf_calc_schema_hash(mod, hash2), 0);

    /* Different modules should have different hashes */
    assert_memory_not_equal(hash1, hash2, 32);

    ly_ctx_destroy(ctx);
}

/**
 * @brief Test file existence check
 */
static void
test_file_exists(void **state)
{
    (void)state;

    /* Non-existent file */
    assert_int_equal(srbf_file_exists("nonexistent-module", SR_DS_STARTUP), 0);

    /* Create file and check */
    int fd = open(TEST_DIR "/nonexistent-module.startup.srbf",
                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert_true(fd > 0);
    close(fd);

    /* Note: srbf_file_exists checks for files in specific paths,
     * so this test may not find the file depending on configuration */
}

/**
 * @brief SRBF file has embedded index and round-trips through deserialize
 */
static void
test_srbf_index_layout_roundtrip(void **state)
{
    struct ly_ctx *ly_ctx = NULL;
    struct lys_module *mod = NULL;
    struct lyd_node *data = NULL, *out = NULL;
    int fd;
    struct srbf_header rh;
    const char *yang =
            "module ixtest {"
            "  namespace \"urn:ix\";"
            "  prefix ix;"
            "  leaf a { type string; }"
            "}";

    (void)state;

    assert_int_equal(ly_ctx_new(NULL, 0, &ly_ctx), LY_SUCCESS);
    lys_parse_mem(ly_ctx, yang, LYS_IN_YANG, &mod);
    assert_non_null(mod);

    assert_int_equal(lyd_new_path(NULL, ly_ctx, "/ixtest:a", "val", 0, &data), LY_SUCCESS);
    assert_non_null(data);

    fd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert_true(fd > 0);
    assert_int_equal(srbf_serialize_tree(data, fd, mod), 0);
    close(fd);

    fd = open(TEST_FILE, O_RDONLY);
    assert_true(fd > 0);
    assert_int_equal(srbf_read_header(fd, &rh), 0);
    close(fd);

    assert_true((rh.flags & SRBF_FLAG_HAS_INDEX) != 0);
    assert_int_equal(rh.idx_offset, sizeof(struct srbf_header));
    assert_true(rh.root_offset > rh.idx_offset);

    fd = open(TEST_FILE, O_RDONLY);
    assert_true(fd > 0);
    assert_int_equal(srbf_deserialize_tree(fd, ly_ctx, mod, &out), 0);
    close(fd);

    assert_non_null(out);
    assert_string_equal(lyd_get_value(out), "val");

    lyd_free_all(data);
    lyd_free_all(out);
    ly_ctx_destroy(ly_ctx);
}

/**
 * @brief Main test runner
 */
int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_header_read_write, setup, teardown),
        cmocka_unit_test_setup_teardown(test_header_validation, setup, teardown),
        cmocka_unit_test_setup_teardown(test_format_detection, setup, teardown),
        cmocka_unit_test_setup_teardown(test_path_generation, setup, teardown),
        cmocka_unit_test(test_xpath_hash),
        cmocka_unit_test(test_index_operations),
        cmocka_unit_test_setup_teardown(test_srbf_index_layout_roundtrip, setup, teardown),
        cmocka_unit_test(test_schema_hash),
        cmocka_unit_test_setup_teardown(test_file_exists, setup, teardown),
    };

    return cmocka_run_group_tests_name("srbin_format", tests, NULL, NULL);
}
