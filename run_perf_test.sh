#!/bin/bash

# 自动查找 build 目录
BUILD_DIR="build"
if [ ! -d "$BUILD_DIR/tests" ]; then
    echo "错误: 找不到 build 目录"
    echo "请在 sysrepo 根目录下运行此脚本"
    exit 1
fi

echo "=== Sysrepo Binary Format 性能测试 ==="
echo "Build 目录: $(pwd)/$BUILD_DIR"
echo ""

echo "测试 1: 宽树结构 (100 个叶子)"
$BUILD_DIR/tests/srbin_perf wide 100 2>&1 | grep -E "wide_100|WRITE:|READ:|SIZE:" | grep -v "libyang"

echo ""
echo "测试 2: 宽树结构 (1000 个叶子)"
$BUILD_DIR/tests/srbin_perf wide 1000 2>&1 | grep -E "wide_1000|WRITE:|READ:|SIZE:" | grep -v "libyang"

echo ""
echo "测试 3: 宽树结构 (10000 个叶子)"
$BUILD_DIR/tests/srbin_perf wide 10000 2>&1 | grep -E "wide_10000|WRITE:|READ:|SIZE:" | grep -v "libyang"

echo ""
echo "测试 4: 简单列表测试"
$BUILD_DIR/tests/test_simple_perf 2>&1 | grep -E "Speedup|Ratio|Average|nodes:"

echo ""
echo "=== 测试完成 ==="
echo "测试文件位置: /tmp/srbin_bench/"
