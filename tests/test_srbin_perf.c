/**
 * @file test_srbin_perf.c
 * @brief Performance benchmarks comparing JSON vs Binary (SRBF) formats
 *
 * Tests focus on:
 * - Deeply nested YANG structures (common in real-world modules)
 * - Wide data structures (many siblings)
 * - Mixed patterns (both deep and wide)
 * - Various data sizes
 *
 * @copyright
 * Copyright (c) 2025 Deutsche Telekom AG.
 * Copyright (c) 2025 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD-3-Clause License (the "License").
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <libyang/libyang.h>

#include "sysrepo.h"
#include "common_srbin.h"
#include "srbin_serialize.h"
#include "srbin_deserialize.h"

#define BENCHMARK_DIR "/tmp/srbin_bench"

/**
 * @brief Benchmark result
 */
struct bench_result {
    double json_time;
    double binary_time;
    double speedup;
};

/**
 * @brief Get current time in seconds
 */
static double
get_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

/**
 * @brief Create a deeply nested YANG module
 *
 * Creates a module with configurable nesting depth, simulating
 * real-world YANG modules like ietf-interfaces, ietf-netconf, etc.
 * that have deep container hierarchies.
 */
static struct lys_module *
create_deep_nested_module(struct ly_ctx *ctx, const char *name, int depth)
{
    size_t yang_def_size = 256 + ((size_t)depth * 220U);
    char *yang_def = malloc(yang_def_size);
    char *p;
    int i;
    struct lys_module *mod = NULL;

    if (!yang_def) {
        return NULL;
    }

    p = yang_def;

    /* Build YANG module with deep nesting */
    p += sprintf(p, "module %s {\n", name);
    p += sprintf(p, "  namespace \"urn:test:%s\";\n", name);
    p += sprintf(p, "  prefix %s;\n\n", name);

    /* Create nested containers */
    for (i = 0; i < depth; i++) {
        p += sprintf(p, "  container level%d {\n", i);
        p += sprintf(p, "    description \"Level %d of nesting\";\n", i);
        p += sprintf(p, "    leaf name {\n");
        p += sprintf(p, "      type string;\n");
        p += sprintf(p, "    }\n");
        p += sprintf(p, "    leaf value {\n");
        p += sprintf(p, "      type int32;\n");
        p += sprintf(p, "    }\n");
    }

    /* Close all containers */
    for (i = 0; i < depth; i++) {
        p += sprintf(p, "  }\n");
    }

    p += sprintf(p, "}\n");

    /* Parse module */
    lys_parse_mem(ctx, yang_def, LYS_IN_YANG, &mod);
    free(yang_def);
    return mod;
}

/**
 * @brief Create a wide YANG module (many siblings)
 *
 * Simulates modules with many top-level nodes like ietf-system,
 * ietf-routing, etc.
 */
static struct lys_module *
create_wide_module(struct ly_ctx *ctx, const char *name, int leaf_count)
{
    /* Allocate enough space: ~200 bytes per leaf + header/footer */
    size_t yang_def_size = 512 + (leaf_count * 200);
    char *yang_def = malloc(yang_def_size);
    char *p = yang_def;
    int i;

    if (!yang_def) {
        return NULL;
    }

    p += sprintf(p, "module %s {\n", name);
    p += sprintf(p, "  namespace \"urn:test:%s\";\n", name);
    p += sprintf(p, "  prefix %s;\n\n", name);
    p += sprintf(p, "  container config {\n");
    p += sprintf(p, "    description \"Configuration with many items\";\n");

    /* Create many leaf nodes */
    for (i = 0; i < leaf_count; i++) {
        p += sprintf(p, "    leaf item%d {\n", i);
        p += sprintf(p, "      type string;\n");
        p += sprintf(p, "      description \"Item %d\";\n", i);
        p += sprintf(p, "    }\n");
    }

    p += sprintf(p, "  }\n");
    p += sprintf(p, "}\n");

    struct lys_module *mod = NULL;
    lys_parse_mem(ctx, yang_def, LYS_IN_YANG, &mod);
    free(yang_def);
    return mod;
}

/**
 * @brief Create a mixed module (both deep and wide)
 *
 * Simulates complex real-world modules.
 */
static struct lys_module *
create_mixed_module(struct ly_ctx *ctx, const char *name, int depth, int width)
{
    /* Allocate enough space for the mixed structure */
    size_t yang_def_size = 512 + (depth * width * 300);
    char *yang_def = malloc(yang_def_size);
    char *p = yang_def;
    int i, j;

    if (!yang_def) {
        return NULL;
    }

    p += sprintf(p, "module %s {\n", name);
    p += sprintf(p, "  namespace \"urn:test:%s\";\n", name);
    p += sprintf(p, "  prefix %s;\n\n", name);

    /* Create wide list at each level */
    for (i = 0; i < depth; i++) {
        p += sprintf(p, "  container level%d {\n", i);
        p += sprintf(p, "    list entries {\n");
        p += sprintf(p, "      key \"id\";\n");
        p += sprintf(p, "      leaf id {\n");
        p += sprintf(p, "        type uint32;\n");
        p += sprintf(p, "      }\n");
        p += sprintf(p, "      leaf name {\n");
        p += sprintf(p, "        type string;\n");
        p += sprintf(p, "      }\n");

        /* Add multiple leaves per list entry */
        for (j = 0; j < width && j < 10; j++) {
            p += sprintf(p, "      leaf param%d {\n", j);
            p += sprintf(p, "        type string;\n");
            p += sprintf(p, "      }\n");
        }

        p += sprintf(p, "    }\n");
        p += sprintf(p, "    leaf description {\n");
        p += sprintf(p, "      type string;\n");
        p += sprintf(p, "    }\n");
    }

    for (i = 0; i < depth; i++) {
        p += sprintf(p, "  }\n");
    }

    p += sprintf(p, "}\n");

    struct lys_module *mod = NULL;
    lys_parse_mem(ctx, yang_def, LYS_IN_YANG, &mod);
    free(yang_def);
    return mod;
}

/**
 * @brief Create test data for deep nesting
 */
static struct lyd_node *
create_deep_data(struct lys_module *mod, int depth)
{
    struct lyd_node *root = NULL;
    struct lyd_node *node = NULL;
    char xpath[512];
    int i;

    /* Create the root level0 container */
    snprintf(xpath, sizeof(xpath), "/%s:level0", mod->name);
    lyd_new_path(NULL, mod->ctx, xpath, NULL, 0, &root);
    if (!root) {
        return NULL;
    }

    /* Set values at root level */
    snprintf(xpath, sizeof(xpath), "/%s:level0/name", mod->name);
    lyd_new_path(root, mod->ctx, xpath, "test-value", 0, NULL);

    snprintf(xpath, sizeof(xpath), "/%s:level0/value", mod->name);
    lyd_new_path(root, mod->ctx, xpath, "42", 0, NULL);

    /* Create nested containers - level1 inside level0, level2 inside level1, etc. */
    for (i = 1; i < depth; i++) {
        /* Build path like /module:level0/level1/.../level{i} */
        xpath[0] = '\0';
        strcat(xpath, "/");
        strcat(xpath, mod->name);
        strcat(xpath, ":level0");
        for (int j = 1; j <= i; j++) {
            strcat(xpath, "/level");
            char num[16];
            snprintf(num, sizeof(num), "%d", j);
            strcat(xpath, num);
        }

        lyd_new_path(root, mod->ctx, xpath, NULL, 0, &node);
    }

    return root;
}

/**
 * @brief Create test data for wide structure
 */
static struct lyd_node *
create_wide_data(struct lys_module *mod, int leaf_count)
{
    struct lyd_node *root = NULL;
    struct lyd_node *node = NULL;
    char xpath[256];
    char value[64];
    int i;

    for (i = 0; i < leaf_count; i++) {
        snprintf(xpath, sizeof(xpath), "/%s:config/item%d", mod->name, i);
        snprintf(value, sizeof(value), "value-for-item-%d", i);
        LY_ERR ret = lyd_new_path(root, mod->ctx, xpath, value, 0, &node);
        if (ret != LY_SUCCESS) {
            fprintf(stderr, "Failed to create node %d: %d\n", i, ret);
        }
        if (i == 0 && node) {
            root = node;
        }
    }

    return root;
}

/**
 * @brief Create test data for mixed structure
 */
static struct lyd_node *
create_mixed_data(struct lys_module *mod, int depth, int entries_per_level)
{
    struct lyd_node *root = NULL;
    struct lyd_node *node = NULL;
    char xpath[512];
    char value[128];
    int i, j;

    /* Create data at each level */
    for (i = 0; i < depth; i++) {
        /* Set description */
        snprintf(xpath, sizeof(xpath), "/%s:level%d/description", mod->name, i);
        snprintf(value, sizeof(value), "Description for level %d", i);
        lyd_new_path(root, mod->ctx, xpath, value, 0, &node);
        if (i == 0 && node) {
            root = node;
        }

        /* Create list entries */
        for (j = 0; j < entries_per_level; j++) {
            snprintf(xpath, sizeof(xpath), "/%s:level%d/entries[id='%d']/name", mod->name, i, j);
            snprintf(value, sizeof(value), "entry-%d-%d", i, j);
            lyd_new_path(root, mod->ctx, xpath, value, 0, NULL);
        }
    }

    return root;
}

/**
 * @brief Benchmark: JSON write
 */
static double
bench_json_write(const struct lyd_node *data, const char *path)
{
    double start, end;
    int fd;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -1;
    }

    start = get_time();
    int ret = lyd_print_fd(fd, data, LYD_JSON, LYD_PRINT_SHRINK);
    fsync(fd);
    end = get_time();

    close(fd);
    if (ret != LY_SUCCESS) {
        return -1;
    }
    return end - start;
}

/**
 * @brief Benchmark: Binary write
 */
static double
bench_binary_write(const struct lyd_node *data, const char *path, const struct lys_module *mod)
{
    double start, end;
    int fd;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -1;
    }

    start = get_time();
    srbf_serialize_tree(data, fd, mod);
    fsync(fd);
    end = get_time();

    close(fd);
    return end - start;
}

/**
 * @brief Benchmark: JSON read (full tree)
 */
static double
bench_json_read(const char *path, struct ly_ctx *ctx, struct lyd_node **data)
{
    double start, end;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    start = get_time();
    lyd_parse_data_fd(ctx, fd, LYD_JSON, LYD_PARSE_STORE_ONLY, 0, data);
    end = get_time();

    close(fd);
    return end - start;
}

/**
 * @brief Benchmark: Binary read (full tree)
 */
static double
bench_binary_read(const char *path, struct ly_ctx *ctx, const struct lys_module *mod, struct lyd_node **data)
{
    double start, end;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    start = get_time();
    srbf_deserialize_tree(fd, ctx, mod, data);
    end = get_time();

    close(fd);
    return end - start;
}

/**
 * @brief Run benchmark comparison
 */
static struct bench_result
run_benchmark(const struct lyd_node *data, const struct lys_module *mod,
              const char *test_name, int iterations)
{
    char json_path[256], bin_path[256];
    struct bench_result result = {0, 0, 0};
    double json_total = 0, bin_total = 0;
    struct lyd_node *read_data = NULL;
    int i;

    printf("\n  Benchmark: %s\n", test_name);
    printf("  Iterations: %d\n", iterations);
    fflush(stdout);

    snprintf(json_path, sizeof(json_path), "%s/%s.json", BENCHMARK_DIR, test_name);
    snprintf(bin_path, sizeof(bin_path), "%s/%s.srbf", BENCHMARK_DIR, test_name);

    /* Warm up run */
    printf("  Warm up: JSON write...\n");
    fflush(stdout);
    bench_json_write(data, json_path);
    printf("  Warm up: Binary write...\n");
    fflush(stdout);
    bench_binary_write(data, bin_path, mod);
    printf("  Warm up complete\n");
    fflush(stdout);

    /* Measure write performance */
    printf("  Measuring write performance...\n");
    fflush(stdout);
    for (i = 0; i < iterations; i++) {
        double t;

        if (i == 0) printf("    Iteration 0: unlink JSON...\n"); fflush(stdout);
        unlink(json_path);

        if (i == 0) printf("    Iteration 0: JSON write...\n"); fflush(stdout);
        t = bench_json_write(data, json_path);
        if (t > 0) {
            json_total += t;
        }

        if (i == 0) printf("    Iteration 0: unlink Binary...\n"); fflush(stdout);
        unlink(bin_path);

        if (i == 0) printf("    Iteration 0: Binary write...\n"); fflush(stdout);
        t = bench_binary_write(data, bin_path, mod);
        if (t > 0) {
            bin_total += t;
        }

        if (i == 0) printf("    Iteration 0 complete\n"); fflush(stdout);
    }
    printf("  Write performance measurement complete\n");
    fflush(stdout);

    result.json_time = json_total / iterations;
    result.binary_time = bin_total / iterations;
    result.speedup = result.json_time / result.binary_time;

    printf("  WRITE: JSON=%.6fs, Binary=%.6fs, Speedup=%.2fx\n",
           result.json_time, result.binary_time, result.speedup);

    /* Measure read performance */
    json_total = 0;
    bin_total = 0;

    printf("  Measuring JSON read performance...\n");
    fflush(stdout);
    for (i = 0; i < iterations; i++) {
        double t;

        lyd_free_all(read_data);
        t = bench_json_read(json_path, data->schema->module->ctx, &read_data);
        if (t > 0) {
            json_total += t;
        }
    }
    printf("  JSON read measurement complete\n");
    fflush(stdout);

    result.json_time = json_total / iterations;

    printf("  Measuring Binary read performance...\n");
    fflush(stdout);
    for (i = 0; i < iterations; i++) {
        double t;

        lyd_free_all(read_data);
        t = bench_binary_read(bin_path, data->schema->module->ctx, mod, &read_data);
        if (t > 0) {
            bin_total += t;
        }
    }
    printf("  Binary read measurement complete\n");
    fflush(stdout);

    result.binary_time = bin_total / iterations;
    result.speedup = result.json_time / result.binary_time;

    printf("  READ:  JSON=%.6fs, Binary=%.6fs, Speedup=%.2fx\n",
           result.json_time, result.binary_time, result.speedup);
    fflush(stdout);

    /* Get file sizes */
    printf("  Getting file sizes...\n");
    fflush(stdout);
    struct stat json_st, bin_st;
    if (stat(json_path, &json_st) == 0 && stat(bin_path, &bin_st) == 0) {
        printf("  SIZE:  JSON=%ld bytes, Binary=%ld bytes, Ratio=%.2f%%\n",
               (long)json_st.st_size, (long)bin_st.st_size,
               (bin_st.st_size * 100.0) / json_st.st_size);
    }
    fflush(stdout);

    printf("  Freeing read_data...\n");
    fflush(stdout);
    lyd_free_all(read_data);
    printf("  Returning from run_benchmark\n");
    fflush(stdout);
    return result;
}

/**
 * @brief Main benchmark suite
 */
int
main(int argc, char **argv)
{
    struct ly_ctx *ctx = NULL;
    struct lys_module *mod = NULL;
    struct lyd_node *data = NULL;
    struct bench_result result;
    int iterations = 100;

    (void)argc;
    (void)argv;

    printf("=== Sysrepo Binary Format Performance Benchmarks ===\n");
    printf("PostgreSQL jsonb-inspired binary format vs JSON\n");
    fflush(stdout);

    /* Create benchmark directory */
    printf("Creating benchmark directory: %s\n", BENCHMARK_DIR);
    fflush(stdout);
    system("mkdir -p " BENCHMARK_DIR);
    printf("Benchmark directory created\n");
    fflush(stdout);

    /* Initialize libyang context */
    if (ly_ctx_new(NULL, 0, &ctx) != LY_SUCCESS) {
        fprintf(stderr, "Failed to create libyang context\n");
        return 1;
    }

    /* Simple API test */
    printf("Testing basic libyang API...\n");
    fflush(stdout);

    struct lyd_node *test_node = NULL;
    LY_ERR ret = lyd_new_path(NULL, ctx, "/test-module:test-container", NULL, 0, &test_node);
    printf("  lyd_new_path returned: %d\n", ret);
    fflush(stdout);

    if (ret == LY_SUCCESS) {
        printf("  lyd_new_path: OK\n");
        lyd_free_all(test_node);
    } else {
        printf("  lyd_new_path: FAILED (expected, module doesn't exist)\n");
    }
    fflush(stdout);

    printf("About to start tests...\n");
    fflush(stdout);

    /*
     * Test 1: Deep nesting (real-world YANG scenario)
     * Modules like ietf-interfaces, ietf-netconf have deep container hierarchies
     */
    printf("TEST 1\n");
    fflush(stdout);
    printf("=== TEST 1: Deep Nesting Performance ===\n");
    fflush(stdout);
    printf("Simulates modules with deep container hierarchies\n");
    fflush(stdout);

    printf("Starting depth tests...\n");
    fflush(stdout);

    int depths[] = {5, 0};
    for (int *d = depths; *d > 0; d++) {
        printf("Processing depth %d\n", *d);
        fflush(stdout);

        char name[64];
        snprintf(name, sizeof(name), "deep_nest_%d", *d);

        printf("Creating module %s...\n", name);
        fflush(stdout);
        mod = create_deep_nested_module(ctx, name, *d);
        printf("Module created: %p\n", (void*)mod);
        fflush(stdout);

        if (!mod) {
            fprintf(stderr, "Failed to create deep module\n");
            continue;
        }

        printf("Creating data...\n");
        fflush(stdout);
        data = create_deep_data(mod, *d);
        printf("Data created: %p\n", (void*)data);
        fflush(stdout);

        printf("\nDepth: %d levels\n", *d);
        mod = create_deep_nested_module(ctx, name, *d);
        if (!mod) {
            fprintf(stderr, "Failed to create deep module\n");
            continue;
        }

        data = create_deep_data(mod, *d);
        if (!data) {
            fprintf(stderr, "Failed to create deep data\n");
            continue;
        }

        result = run_benchmark(data, mod, name, iterations);

        lyd_free_all(data);
        data = NULL;
        mod = NULL;
    }

    /*
     * Test 2: Wide structures (many siblings)
     * Simulates modules with many configuration items
     */
    printf("\n=== TEST 2: Wide Structure Performance ===\n");
    printf("Simulates modules with many top-level items\n");

    int widths[] = {100, 1000, 10000, 0};
    for (int *w = widths; *w > 0; w++) {
        char name[64];
        snprintf(name, sizeof(name), "wide_%d", *w);

        printf("\nWidth: %d items\n", *w);
        mod = create_wide_module(ctx, name, *w);
        if (!mod) {
            fprintf(stderr, "Failed to create wide module\n");
            continue;
        }

        data = create_wide_data(mod, *w);
        if (!data) {
            fprintf(stderr, "Failed to create wide data\n");
            continue;
        }

        result = run_benchmark(data, mod, name, iterations / 10 + 10);

        lyd_free_all(data);
        data = NULL;
        mod = NULL;
    }

    /*
     * Test 3: Mixed structures (real-world complexity)
     * Simulates complex modules with both deep and wide patterns
     */
    printf("\n=== TEST 3: Mixed Structure Performance ===\n");
    printf("Simulates complex real-world modules\n");

    struct {
        int depth;
        int width;
        int iter;
    } mixed_tests[] = {
        {5, 10, 100},
        {10, 20, 50},
        {15, 50, 20},
        {0, 0, 0}
    };

    for (int i = 0; mixed_tests[i].depth > 0; i++) {
        char name[64];
        snprintf(name, sizeof(name), "mixed_d%d_w%d",
                 mixed_tests[i].depth, mixed_tests[i].width);

        printf("\nDepth: %d, Width: %d\n", mixed_tests[i].depth, mixed_tests[i].width);

        mod = create_mixed_module(ctx, name, mixed_tests[i].depth, mixed_tests[i].width);
        if (!mod) {
            fprintf(stderr, "Failed to create mixed module\n");
            continue;
        }

        data = create_mixed_data(mod, mixed_tests[i].depth, mixed_tests[i].width);
        if (!data) {
            fprintf(stderr, "Failed to create mixed data\n");
            continue;
        }

        result = run_benchmark(data, mod, name, mixed_tests[i].iter);

        lyd_free_all(data);
        data = NULL;
        mod = NULL;
    }

    /*
     * Test 4: Extreme deep nesting (stress test)
     * Tests the limits of the implementation
     */
    printf("\n=== TEST 4: Extreme Deep Nesting (Stress Test) ===\n");

    int extreme_depths[] = {30, 50, 0};
    for (int *d = extreme_depths; *d > 0; d++) {
        char name[64];
        snprintf(name, sizeof(name), "extreme_deep_%d", *d);

        printf("\nExtreme Depth: %d levels\n", *d);
        mod = create_deep_nested_module(ctx, name, *d);
        if (!mod) {
            fprintf(stderr, "Failed to create extreme deep module\n");
            continue;
        }

        data = create_deep_data(mod, *d);
        if (!data) {
            fprintf(stderr, "Failed to create extreme deep data\n");
            continue;
        }

        result = run_benchmark(data, mod, name, 10);

        lyd_free_all(data);
        data = NULL;
        mod = NULL;
    }

    /* Cleanup */
    ly_ctx_destroy(ctx);

    printf("\n=== Benchmarks Complete ===\n");
    printf("Results summary:\n");
    printf("  - Binary format should show significant read speedup\n");
    printf("  - Deep nesting benefits most from binary format\n");
    printf("  - File size reduction expected (20-40%%)\n");

    return 0;
}
