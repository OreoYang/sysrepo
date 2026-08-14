#!/bin/bash

# 自动查找 build 目录
BUILD_DIR="build"
if [ ! -d "$BUILD_DIR/tests" ]; then
    echo "错误: 找不到 build 目录"
    echo "请在 sysrepo 根目录下运行此脚本"
    exit 1
fi

echo "=== 快速性能测试 (~10秒) ==="
echo "Build 目录: $(pwd)/$BUILD_DIR"
echo ""

echo "测试 wide_100..."
$BUILD_DIR/tests/srbin_perf wide 100 2>&1 | grep -A 10 "wide_100"

echo ""
echo "=== 测试完成 ==="
echo "测试文件位置: /tmp/srbin_bench/"
