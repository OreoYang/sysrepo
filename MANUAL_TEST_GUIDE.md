# Sysrepo Binary Format 手动测试指南

## 目录说明

```
/home/oreo/works/private/sysrepo/
├── build/                    # 编译目录
│   └── tests/
│       ├── srbin_perf        # 综合性能测试
│       └── test_simple_perf  # 简单列表测试
├── run_perf_test.sh         # 完整测试脚本 (从根目录执行)
└── run_quick_test.sh        # 快速测试脚本 (从根目录执行)
```

## 测试方式

### 方法一：从根目录执行（推荐）

```bash
cd /home/oreo/works/private/sysrepo

# 完整测试 (约2-3分钟)
./run_perf_test.sh

# 快速测试 (约10秒)
./run_quick_test.sh
```

### 方法二：直接运行测试程序

```bash
cd /home/oreo/works/private/sysrepo

# 宽树结构测试
build/tests/srbin_perf wide 100    # 小规模
build/tests/srbin_perf wide 1000   # 中等规模
build/tests/srbin_perf wide 10000  # 大规模

# 深树结构测试
build/tests/srbin_perf deep 10
build/tests/srbin_perf deep 20

# 简单列表测试
build/tests/test_simple_perf
```

### 方法三：从 build 目录执行

```bash
cd /home/oreo/works/private/sysrepo/build

# 使用相对路径
./tests/srbin_perf wide 1000
./tests/test_simple_perf
```

## 结果解读

### 输出示例

```
  WRITE: JSON=0.004056s, Binary=0.000133s, Speedup=30.59x
  READ:  JSON=0.045520s, Binary=0.000003s, Speedup=17278.19x
  SIZE:  JSON=30802 bytes, Binary=62022 bytes, Ratio=201.36%
```

### 含义

- **WRITE**: 写入性能，Speedup 越大越好
- **READ**: 读取性能，Speedup 越大越好（通常比写入快很多）
- **SIZE**: 文件大小，Ratio 越小越好（Binary 通常比 JSON 大 2-2.5x）

## 测试文件位置

- 测试数据: `/tmp/srbin_bench/`
- 简单测试数据: `/tmp/simple_bench/`

## 验证 Binary 格式

```bash
# 检查魔数 (应该是 SRBF)
xxd /tmp/srbin_bench/wide_1000.srbf | head -1

# 对比文件大小
ls -lh /tmp/srbin_bench/wide_1000.*
```
