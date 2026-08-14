/**
 * @file test_olt_json_perf.c
 * @brief OLT Config JSON Performance Test
 *
 * Tests JSON read/write performance using a real-world OLT configuration file.
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
 */
static const char *olt_modules[] = {
    /* Core modules */
    "ietf-interfaces",
    "ietf-ip",
    "ietf-yang-types",
    "ietf-inet-types",
    "iana-if-type",
    /* BBF modules */
    "bbf-l2-dhcpv4-relay",
    "bbf-l2-forwarding",
    "bbf-ldra",
    "bbf-onus",
    "bbf-onu-management",
    "bbf-qos-policies",
    "bbf-qos-shaping",
    "bbf-qos-traffic-mngt",
    "bbf-hardware",
    "bbf-interface-usage",
    "bbf-dot1q-types",
    "bbf-frame-classification",
    "bbf-xpon",
    "bbf-xpon-types",
    "bbf-xpon-if-type",
    "bbf-yang-types",
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
    NULL
};

/**
 * @brief Load required YANG modules
 */
static int load_olt_modules(struct ly_ctx *ctx)
{
    int i, loaded = 0;

    printf("Loading YANG modules...\n");
    for (i = 0; olt_modules[i] != NULL; i++) {
        const struct lys_module *mod = ly_ctx_load_module(ctx, olt_modules[i], NULL, 0);
        if (mod) {
            loaded++;
        } else {
            /* Try with different revision or silently skip */
            printf("  Note: module '%s' not found\n", olt_modules[i]);
        }
    }
    printf("Loaded %d modules\n\n", loaded);
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
    lyd_parse_data_fd(ctx, fd, LYD_JSON, LYD_PARSE_STORE_ONLY | LYD_PARSE_OPAQ, 0, &read_data);
    end = get_time();
    close(fd);

    double read_time = end - start;

    if (read_data) lyd_free_all(read_data);

    printf("    JSON: write=%.6fs, read=%.6fs, total=%.6fs\n", write_time, read_time, write_time + read_time);
    return write_time + read_time;
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

int main(int argc, char **argv)
{
    struct ly_ctx *ctx = NULL;
    struct lyd_node *olt_data = NULL;
    char json_path[256];
    int iterations = 10;
    int i;

    (void)argc;
    (void)argv;

    printf("=== OLT Config JSON Performance Test ===\n");
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
    load_olt_modules(ctx);

    /* Load OLT config from XML */
    printf("Loading OLT config from XML...\n");

    FILE *f = fopen(OLT_CONFIG_FILE, "r");
    if (!f) {
        fprintf(stderr, "Failed to open %s\n", OLT_CONFIG_FILE);
        ly_ctx_destroy(ctx);
        return 1;
    }

    /* Read file content */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *xml_content = malloc(size + 1);
    fread(xml_content, 1, size, f);
    xml_content[size] = '\0';
    fclose(f);

    /* Parse with OPAQ flag to handle data without full schema validation */
    LY_ERR ret = lyd_parse_data_mem(ctx, xml_content, LYD_XML, LYD_PARSE_ONLY | LYD_PARSE_OPAQ, 0, &olt_data);
    free(xml_content);

    if (ret != LY_SUCCESS || !olt_data) {
        fprintf(stderr, "Failed to parse OLT config XML, error code: %d\n", ret);
        ly_ctx_destroy(ctx);
        return 1;
    }

    int node_count = count_nodes(olt_data);
    printf("Loaded %d data nodes\n\n", node_count);

    /* Setup file path */
    snprintf(json_path, sizeof(json_path), "%s/olt_config.json", BENCHMARK_DIR);

    /* Warm up */
    printf("Warm up run...\n");
    bench_json_operations(olt_data, json_path, ctx);
    printf("\n");

    /* Benchmark */
    printf("Running %d iterations...\n\n", iterations);

    double json_total = 0;

    for (i = 0; i < iterations; i++) {
        double t;

        printf("Iteration %d:\n", i + 1);

        t = bench_json_operations(olt_data, json_path, ctx);
        if (t > 0) json_total += t;

        printf("\n");
    }

    /* Calculate averages */
    double json_avg = json_total / iterations;

    /* Get file size */
    long json_size = get_file_size(json_path);

    /* Print summary */
    printf("=== RESULTS ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Data nodes: %d\n", node_count);
    printf("\nAverage times:\n");
    printf("  JSON write+read: %.6f seconds\n", json_avg);
    printf("\nFile size:\n");
    printf("  JSON: %ld bytes (%.2f KB)\n", json_size, json_size / 1024.0);

    /* Cleanup */
    lyd_free_all(olt_data);
    ly_ctx_destroy(ctx);

    return 0;
}
