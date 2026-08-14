/**
 * @file test_simple_perf.c
 * @brief Simple performance test with generated data
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <libyang/libyang.h>

#include "sysrepo.h"
#include "srbin_serialize.h"
#include "srbin_deserialize.h"

#define BENCHMARK_DIR "/tmp/simple_bench"

static double get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

static int count_nodes(const struct lyd_node *root)
{
    int count = 0;
    const struct lyd_node *node = root;

    while (node) {
        count++;
        struct lyd_node *child = lyd_child(node);
        while (child) {
            count += count_nodes(child);
            child = child->next;
        }
        node = node->next;
    }
    return count;
}

/* Create a simple test data tree using lyd_new_path */
static struct lyd_node *create_test_data(struct ly_ctx *ctx, int num_items)
{
    struct lys_module *mod;
    struct lyd_node *root = NULL;
    char path[512];
    int i;

    /* Create simple module */
    const char *yang_def =
        "module test-perf {"
        "  namespace \"urn:test:perf\";"
        "  prefix tp;"
        "  container data {"
        "    list items {"
        "      key \"name\";"
        "      leaf name { type string; }"
        "      leaf value { type int32; }"
        "      leaf desc { type string; }"
        "    }"
        "  }"
        "}";

    if (lys_parse_mem(ctx, yang_def, LYS_IN_YANG, &mod) != LY_SUCCESS) {
        fprintf(stderr, "Failed to parse module\n");
        return NULL;
    }

    /* Create all list items - pass root as parent each time */
    for (i = 0; i < num_items; i++) {
        /* Create list entry with value */
        snprintf(path, sizeof(path),
            "/test-perf:data/items[name='item-%d']/value", i);
        lyd_new_path(root, ctx, path, "1000", LYD_NEW_PATH_UPDATE, NULL);

        /* Create list entry with desc */
        snprintf(path, sizeof(path),
            "/test-perf:data/items[name='item-%d']/desc", i);
        char desc[128];
        snprintf(desc, sizeof(desc), "Description for item %d with additional text data", i);
        lyd_new_path(root, ctx, path, desc, LYD_NEW_PATH_UPDATE, NULL);

        /* After first creation, get the root to link subsequent items */
        if (i == 0) {
            snprintf(path, sizeof(path), "/test-perf:data/items[name='item-%d']", i);
            lyd_new_path(NULL, ctx, path, NULL, LYD_NEW_PATH_UPDATE, &root);
        }
    }

    return root;
}

static double bench_json(struct lyd_node *data, const char *path)
{
    double start, end;
    int fd;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    start = get_time();
    lyd_print_fd(fd, data, LYD_JSON, LYD_PRINT_SHRINK);
    fsync(fd);
    end = get_time();
    close(fd);
    double write_time = end - start;

    struct lyd_node *read_data = NULL;
    fd = open(path, O_RDONLY);
    start = get_time();
    lyd_parse_data_fd(data->schema->module->ctx, fd, LYD_JSON, 0, 0, &read_data);
    end = get_time();
    close(fd);
    double read_time = end - start;

    if (read_data) lyd_free_all(read_data);

    printf("    JSON: write=%.6fs, read=%.6fs, total=%.6fs\n", write_time, read_time, write_time + read_time);
    return write_time + read_time;
}

static double bench_binary(struct lyd_node *data, const char *path)
{
    double start, end;
    int fd;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    start = get_time();
    srbf_serialize_tree(data, fd, data->schema->module);
    fsync(fd);
    end = get_time();
    close(fd);
    double write_time = end - start;

    struct lyd_node *read_data = NULL;
    fd = open(path, O_RDONLY);
    start = get_time();
    srbf_deserialize_tree(fd, data->schema->module->ctx, data->schema->module, &read_data);
    end = get_time();
    close(fd);
    double read_time = end - start;

    if (read_data) lyd_free_all(read_data);

    printf("    Binary: write=%.6fs, read=%.6fs, total=%.6fs\n", write_time, read_time, write_time + read_time);
    return write_time + read_time;
}

static long get_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return st.st_size;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct ly_ctx *ctx = NULL;
    struct lyd_node *data = NULL;
    char json_path[256], bin_path[256];
    int num_items = 1000;
    int iterations = 10;
    int i;

    (void)argc;
    (void)argv;

    printf("=== Simple Data Performance Test ===\n");
    printf("Items: %d, Iterations: %d\n\n", num_items, iterations);

    system("mkdir -p " BENCHMARK_DIR);

    ly_ctx_new(NULL, 0, &ctx);

    printf("Creating test data...\n");
    data = create_test_data(ctx, num_items);
    if (!data) {
        fprintf(stderr, "Failed to create test data\n");
        return 1;
    }

    int node_count = count_nodes(data);
    printf("Created %d nodes\n\n", node_count);

    snprintf(json_path, sizeof(json_path), "%s/test_data.json", BENCHMARK_DIR);
    snprintf(bin_path, sizeof(bin_path), "%s/test_data.srbf", BENCHMARK_DIR);

    /* Warm up */
    printf("Warm up...\n");
    bench_json(data, json_path);
    bench_binary(data, bin_path);
    printf("\n");

    /* Benchmark */
    printf("Running %d iterations...\n\n", iterations);

    double json_total = 0, bin_total = 0;
    for (i = 0; i < iterations; i++) {
        printf("Iteration %d:\n", i + 1);
        json_total += bench_json(data, json_path);
        bin_total += bench_binary(data, bin_path);
        printf("\n");
    }

    /* Get file sizes */
    long json_size = get_file_size(json_path);
    long bin_size = get_file_size(bin_path);

    printf("=== RESULTS ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Data nodes: %d\n", node_count);
    printf("\nAverage times:\n");
    printf("  JSON:   %.6f seconds\n", json_total / iterations);
    printf("  Binary: %.6f seconds\n", bin_total / iterations);
    printf("  Speedup: %.2fx faster\n", json_total / bin_total);
    printf("\nFile sizes:\n");
    printf("  JSON:   %ld bytes (%.2f KB)\n", json_size, json_size / 1024.0);
    printf("  Binary: %ld bytes (%.2f KB)\n", bin_size, bin_size / 1024.0);
    printf("  Ratio:  %.2f%%\n", (bin_size * 100.0) / json_size);

    lyd_free_all(data);
    ly_ctx_destroy(ctx);

    return 0;
}
