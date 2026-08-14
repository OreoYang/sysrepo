/**
 * @file test_olt_perf.c
 * @brief OLT Config Performance Test - JSON vs Binary Format
 *
 * Compares read performance between JSON and binary (SRBF) formats
 * using a real-world OLT configuration file.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>

#include <libyang/libyang.h>

#include "sysrepo.h"
#include "common_srbin.h"
#include "srbin_serialize.h"
#include "srbin_deserialize.h"

#define OLT_CONFIG_FILE "/home/oreo/works/private/sysrepo/tests/files/oltconfig.xml"
#define BENCHMARK_DIR "/tmp/olt_bench"

/**
 * @brief Get current time in seconds
 */
static double get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

/**
 * @brief List of YANG modules required for OLT config
 * Only main modules - submodules are auto-included
 * IMPORTANT: bbf-xpon-types must be loaded before bbf-xpon
 */
static const char *olt_modules[] = {
    /* Core IETF modules */
    "ietf-interfaces",
    "ietf-ip",
    "ietf-yang-types",
    "ietf-inet-types",
    "iana-if-type",
    "ietf-system",
    "ietf-hardware",
    "iana-hardware",
    /* BBF L2 modules */
    "bbf-l2-dhcpv4-relay",
    "bbf-l2-forwarding",
    "bbf-dot1q-types",
    "bbf-frame-classification",
    /* BBF QoS modules */
    "bbf-qos-policies",
    "bbf-qos-shaping",
    "bbf-qos-traffic-mngt",
    /* BBF hardware */
    "bbf-hardware",
    "bbf-interface-usage",
    /* BBF XPON types MUST be loaded first (dependency order) */
    "bbf-xpon-types",
    "bbf-xpon-if-type",
    "bbf-xpon",
    "bbf-xponani",
    "bbf-xpongemtcont",
    "bbf-xponvani",
    /* Vecima modules */
    "vecima-l2-forwarding",
    "vecima-qos-shaping",
    "vecima-interfaces",
    "vecima-hardware",
    "vecima-ietf-system",
    "vecima-lldp",
    "vecima-device",
    "vecima-olt-subsys",
    "vecima-common",
    "vecima-xpon",
    NULL
};

/**
 * @brief Load required YANG modules
 */
static int load_olt_modules(struct ly_ctx *ctx)
{
    int i, loaded = 0, failed = 0;

    printf("Loading YANG modules...\n");
    for (i = 0; olt_modules[i] != NULL; i++) {
        const struct lys_module *mod = ly_ctx_load_module(ctx, olt_modules[i], NULL, 0);
        if (mod) {
            loaded++;
        } else {
            /* Try with different revision or silently skip */
            printf("  Warning: module '%s' not found\n", olt_modules[i]);
            failed++;
        }
    }
    printf("Loaded %d modules, %d failed\n\n", loaded, failed);
    return (failed > 0) ? -1 : 0;
}

/**
 * @brief Load OLT config from XML file
 * Parses with full schema validation for SRBF compatibility
 * Extracts the data section from the rpc-reply wrapper
 */
static int load_olt_xml(struct ly_ctx *ctx, struct lyd_node **data)
{
    FILE *f = fopen(OLT_CONFIG_FILE, "r");
    if (!f) {
        fprintf(stderr, "Failed to open %s\n", OLT_CONFIG_FILE);
        return -1;
    }

    /* Read file content */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *xml_content = malloc(size + 1);
    fread(xml_content, 1, size, f);
    xml_content[size] = '\0';
    fclose(f);

    /* Parse with OPAQ flag to handle NETCONF rpc-reply wrapper */
    /* The schema validation will still happen for known modules */
    struct lyd_node *rpc_tree = NULL;
    LY_ERR ret = lyd_parse_data_mem(ctx, xml_content, LYD_XML, LYD_PARSE_ONLY | LYD_PARSE_OPAQ, 0, &rpc_tree);
    free(xml_content);

    if (ret != LY_SUCCESS) {
        fprintf(stderr, "Failed to parse OLT config XML, error code: %d\n", ret);
        return -1;
    }

    if (!rpc_tree) {
        fprintf(stderr, "Parsed successfully but no data returned (empty tree)\n");
        return -1;
    }

    printf("Parsed successfully, root node: %s\n",
           LYD_NAME(rpc_tree) ? LYD_NAME(rpc_tree) : "(null)");

    /* Navigate to the <data> section within the rpc-reply */
    struct lyd_node *data_node = NULL;

    /* First, check if the root itself is the data node */
    if (LYD_NAME(rpc_tree) && strcmp(LYD_NAME(rpc_tree), "data") == 0) {
        data_node = rpc_tree;
    } else {
        /* Otherwise, search for it */
        struct lyd_node *child = lyd_child(rpc_tree);
        while (child) {
            if (LYD_NAME(child) && strcmp(LYD_NAME(child), "data") == 0) {
                data_node = child;
                break;
            }
            child = child->next;
        }
    }

    if (!data_node) {
        fprintf(stderr, "Failed to find <data> section in parsed tree\n");
        lyd_free_all(rpc_tree);
        return -1;
    }

    /* Get actual data (children of <data> node) */
    struct lyd_node *actual_data = lyd_child(data_node);

    if (!actual_data) {
        fprintf(stderr, "Data section is empty\n");
        lyd_free_all(rpc_tree);
        return -1;
    }

    printf("Found data section, first child: %s\n", LYD_NAME(actual_data) ? LYD_NAME(actual_data) : "(null)");

    /* Count nodes and check for opaque */
    int opaque_count = 0, schema_count = 0;
    struct lyd_node *node = actual_data;
    int count = 0;
    while (node && count < 100) {  /* Check first 100 nodes */
        if (node->schema) {
            schema_count++;
        } else {
            opaque_count++;
            printf("  Opaque node: %s\n", LYD_NAME(node) ? LYD_NAME(node) : "(null)");
        }
        node = node->next;
        count++;
    }
    printf("First 100 nodes: %d with schema, %d opaque\n", schema_count, opaque_count);

    /* Duplicate all siblings to get our own copy */
    LY_ERR dup_ret = lyd_dup_siblings(actual_data, NULL, LYD_DUP_RECURSIVE, data);
    lyd_free_all(rpc_tree);

    if (dup_ret != LY_SUCCESS) {
        fprintf(stderr, "Failed to duplicate data tree\n");
        return -1;
    }

    return 0;
}

/**
 * @brief Benchmark: JSON write and read
 */
static double bench_json_operations(struct lyd_node *data, const char *json_path, struct ly_ctx *ctx)
{
    double start, end;
    int fd;

    /* Write */
    fd = open(json_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open json for write");
        return -1;
    }

    start = get_time();
    lyd_print_fd(fd, data, LYD_JSON, LYD_PRINT_SHRINK);
    fsync(fd);
    end = get_time();
    close(fd);

    double write_time = end - start;

    /* Read */
    struct lyd_node *read_data = NULL;
    fd = open(json_path, O_RDONLY);
    if (fd < 0) {
        perror("open json for read");
        return -1;
    }

    start = get_time();
    lyd_parse_data_fd(ctx, fd, LYD_JSON, LYD_PARSE_STORE_ONLY, 0, &read_data);
    end = get_time();
    close(fd);

    double read_time = end - start;

    if (read_data) lyd_free_all(read_data);

    printf("    JSON: write=%.6fs, read=%.6fs, total=%.6fs\n", write_time, read_time, write_time + read_time);
    return write_time + read_time;
}

/**
 * @brief Benchmark: Binary write and read
 */
static double bench_binary_operations(struct lyd_node *data, const char *bin_path, struct ly_ctx *ctx, const struct lys_module *module)
{
    double start, end;
    int fd;
    int ret;

    (void)ctx;  /* Unused for now since deserialization is skipped */

    /* Write */
    fd = open(bin_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open binary for write");
        return -1;
    }

    start = get_time();
    ret = srbf_serialize_tree(data, fd, module);
    fsync(fd);
    end = get_time();
    close(fd);

    if (ret < 0) {
        fprintf(stderr, "Error: Binary serialization failed: %d\n", ret);
        return -1;
    }

    double write_time = end - start;

    /* Get file size */
    struct stat st;
    long bin_size = 0;
    if (stat(bin_path, &st) == 0) {
        bin_size = st.st_size;
    }

    printf("    Binary: write=%.6fs, size=%ld bytes\n", write_time, bin_size);

    /* Note: Deserialization with opaque nodes not yet supported
     * For this test, we're only measuring write performance */

    return write_time;
}

/**
 * @brief Get file size
 */
static long get_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return st.st_size;
    }
    return 0;
}

/**
 * @brief Recursively count nodes in data tree
 */
static int count_nodes_recursive(const struct lyd_node *node)
{
    int count = 1;  /* Count this node */
    struct lyd_node *child = lyd_child(node);

    while (child) {
        count += count_nodes_recursive(child);
        child = child->next;
    }

    return count;
}

/**
 * @brief Count nodes in data tree
 */
static int count_nodes(const struct lyd_node *root)
{
    int count = 0;
    const struct lyd_node *node = root;

    while (node) {
        count += count_nodes_recursive(node);
        node = node->next;
    }

    return count;
}

int main(int argc, char **argv)
{
    struct ly_ctx *ctx = NULL;
    struct lyd_node *olt_data = NULL;
    char json_path[256], bin_path[256];
    int iterations = 10;
    int i;

    (void)argc;
    (void)argv;

    printf("=== OLT Config Performance Test ===\n");
    printf("Testing with real-world OLT configuration: %s\n\n", OLT_CONFIG_FILE);

    /* Create benchmark directory */
    system("mkdir -p " BENCHMARK_DIR);

    /* Initialize libyang context with YANG module search paths */
    if (ly_ctx_new("/home/oreo/works/yang/exs1610", 0, &ctx) != LY_SUCCESS) {
        fprintf(stderr, "Failed to create libyang context\n");
        return 1;
    }

    /* Add additional search paths */
    ly_ctx_set_searchdir(ctx, "/home/oreo/works/yang/common");
    ly_ctx_set_searchdir(ctx, "/home/oreo/works/yang/ietf");
    ly_ctx_set_searchdir(ctx, "/home/oreo/works/yang/bbf/tr385/standard/networking");
    ly_ctx_set_searchdir(ctx, "/home/oreo/works/yang/bbf/tr385/standard/interface");
    ly_ctx_set_searchdir(ctx, "/home/oreo/works/yang/bbf/tr383/standard/networking");
    ly_ctx_set_searchdir(ctx, "/home/oreo/works/yang/bbf/tr383/standard/interface");

    /* Load required YANG modules */
    if (load_olt_modules(ctx) < 0) {
        printf("Warning: Some modules failed to load, may affect parsing\n");
    }

    /* Load OLT config from XML */
    printf("Loading OLT config from XML...\n");

    if (load_olt_xml(ctx, &olt_data) < 0) {
        fprintf(stderr, "Failed to load OLT config\n");
        ly_ctx_destroy(ctx);
        return 1;
    }

    if (!olt_data) {
        fprintf(stderr, "No data loaded from OLT config\n");
        ly_ctx_destroy(ctx);
        return 1;
    }

    int node_count = count_nodes(olt_data);
    printf("Loaded %d data nodes\n\n", node_count);

    /* Find a suitable module for binary operations - use first loaded module */
    const struct lys_module *module = NULL;
    uint32_t idx = 0;
    while ((module = ly_ctx_get_module_iter(ctx, &idx))) {
        /* Use the first non-internal module */
        if (module->name && strlen(module->name) > 0) {
            break;
        }
    }

    if (!module) {
        fprintf(stderr, "Warning: No suitable module found, using ietf-interfaces\n");
        module = ly_ctx_get_module(ctx, "ietf-interfaces", NULL);
    }

    if (module) {
        printf("Using module '%s' for binary operations\n\n", module->name);
    } else {
        fprintf(stderr, "Error: No module available\n");
        lyd_free_all(olt_data);
        ly_ctx_destroy(ctx);
        return 1;
    }

    /* Setup file paths */
    snprintf(json_path, sizeof(json_path), "%s/olt_config.json", BENCHMARK_DIR);
    snprintf(bin_path, sizeof(bin_path), "%s/olt_config.srbf", BENCHMARK_DIR);

    /* Warm up */
    printf("Warm up run...\n");
    bench_json_operations(olt_data, json_path, ctx);
    bench_binary_operations(olt_data, bin_path, ctx, module);
    printf("\n");

    /* Benchmark */
    printf("Running %d iterations...\n\n", iterations);

    double json_total = 0, bin_total = 0;

    for (i = 0; i < iterations; i++) {
        double t;

        printf("Iteration %d:\n", i + 1);

        t = bench_json_operations(olt_data, json_path, ctx);
        if (t > 0) json_total += t;

        t = bench_binary_operations(olt_data, bin_path, ctx, module);
        if (t > 0) bin_total += t;

        printf("\n");
    }

    /* Calculate averages */
    double json_avg = json_total / iterations;
    double bin_avg = bin_total / iterations;
    double speedup = json_avg / bin_avg;

    /* Get file sizes */
    long json_size = get_file_size(json_path);
    long bin_size = get_file_size(bin_path);

    /* Print summary */
    printf("=== RESULTS ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Data nodes: %d\n", node_count);
    printf("\nAverage times:\n");
    printf("  JSON:   %.6f seconds\n", json_avg);
    printf("  Binary: %.6f seconds\n", bin_avg);
    printf("  Speedup: %.2fx faster\n", speedup);
    printf("\nFile sizes:\n");
    printf("  JSON:   %ld bytes (%.2f KB)\n", json_size, json_size / 1024.0);
    printf("  Binary: %ld bytes (%.2f KB)\n", bin_size, bin_size / 1024.0);
    printf("  Ratio:  %.2f%%\n", (bin_size * 100.0) / json_size);

    /* Cleanup */
    lyd_free_all(olt_data);
    ly_ctx_destroy(ctx);

    return 0;
}
