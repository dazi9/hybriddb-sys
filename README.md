# HybridDB — 面向系统原理与论文实验的 B+ 树 / LSM-Tree 双引擎存储系统 (C++20)

`HybridDB` 是专为计算机专业大二学生结合 **《深入理解计算机系统 (CS:APP)》** 设计的系统软件研究与实现项目。

不同于简单的课程实验（如 CS:APP Malloc/Cache/Shell Lab）或套壳 Web 应用，本项目直接从操作系统底层 I/O、内存层次结构与经典数据库系统论文出发，使用 **现代 C++20（零第三方依赖，`g++ -std=c++20 -O2 -Wall -Wextra -Werror -pthread`）** 从零实现了同一套 4KB 分页与 WAL 基础设施之上的 **B+ 树存储引擎** 与 **LSM-Tree 存储引擎**，并完成了对两篇 ACM SIGMOD 顶会论文核心算法的复现与控制变量实验验证：
1. **O'Neil, O'Neil, Weikum (ACM SIGMOD 1993)**：*The LRU-K Page Replacement Algorithm For Database Disk Buffering* —— 复现带历史保留期（Retained Information Period）的 **`LRU-2` 缓冲池替换算法**，在周期性顺序扫描污染（Scan Pollution）下相比传统 `LRU-1` 与 `Clock` 将缓存命中率从 **39.59% 提升至 61.25%**。
2. **Dayan, Athanassoulis, Idreos (ACM SIGMOD 2017)**：*Monkey: Optimal Navigable Key-Value Store* —— 复现 **Monkey 跨层级非均匀 Bloom Filter 比特分配算法**，在完全相同的内存预算下相比工业界默认均匀分配（Uniform）将零结果点查的无效磁盘页读取降低了 **23.0% ~ 38.8%**。

---

## 目录结构

```text
hybriddb-sys/
├── Makefile                                            # C++20 编译、测试、论文对比实验构建脚本
├── LICENSE                                             # MIT 许可证
├── README.md                                           # 项目总览与快速指南
├── include/hybriddb/
│   ├── common.hpp                                      # 状态码、统计指标、IEEE CRC32、MurmurHash3/FNV-1a
│   ├── page.hpp                                        # 4096B SlottedPage 分槽页布局与 DiskManager (pread/pwrite)
│   ├── buffer_pool.hpp                                 # 缓冲池管理器与 LRU-1 / Clock / LRU-2 可插拔替换器
│   ├── wal.hpp                                         # 预写日志 WALManager、CRC32 校验与故障注入接口
│   ├── bloom.hpp                                       # Kirsch-Mitzenmacher 双哈希布隆过滤器与 Monkey 最优分配器
│   ├── btree.hpp                                       # 基于缓冲池的磁盘 B+ 树存储引擎 (BPlusTreeEngine)
│   └── lsm.hpp                                         # SkipList MemTable + 多层 SSTable + Compaction (LSMTreeEngine)
├── src/
│   ├── disk_manager.cpp                                # POSIX pread/pwrite/fsync 4KB 页管理实现
│   ├── buffer_pool.cpp                                 # LRU-1、Clock、LRU-2 替换策略与缓冲池实现
│   ├── wal.cpp                                         # 预写日志追加、截断自愈与故障注入实现
│   ├── bloom.cpp                                       # 双哈希布隆过滤器与 Monkey 拉格朗日最优比特求解实现
│   ├── btree.cpp                                       # B+ 树内部节点/叶子节点分裂、范围扫描与 WAL 恢复实现
│   ├── lsm.cpp                                         # 跳表、SSTable 稀疏 Fence Pointers、多路归并 Compaction
│   └── cli_main.cpp                                    # 命令行与交互式 REPL 工具 (hybriddb_cli)
├── tests/
│   └── test_main.cpp                                   # 12 大自动化正确性、边界、并发与故障注入测试套件
├── benchmarks/
│   └── bench_experiments.cpp                           # 4 组控制变量论文验证实验（自动生成 CSV 数据）
├── results/                                            # 实测生成的标准 CSV 实验结果文件
│   ├── exp1_btree_vs_lsm.csv
│   ├── exp2_buffer_pool_policies.csv
│   ├── exp3_monkey_bloom_tuning.csv
│   └── exp4_wal_crash_recovery.csv
└── docs/
    ├── stage1_to_4_research_and_papers.md              # 阶段一至四：6个开源项目源码核验 + 6篇论文精读与选型报告
    └── stage5_to_7_architecture_and_semester_plan.md   # 阶段五至七：5层架构详解、实验分析、学期计划与答辩指南
```

---

## 编译、测试与运行指南

### 1. 环境要求
- 编译器：`g++ >= 10`（支持 `-std=c++20`，已在 `g++ 15.2.0` Ubuntu/WSL 下严格验证）
- 构建工具：`make`
- 外部依赖：**零第三方依赖**（仅使用 C++20 标准库与 POSIX 系统调用 `open`/`pread`/`pwrite`/`fsync`/`ftruncate`）

### 2. 一键编译与运行自动化测试
```bash
cd /mnt/c/Users/18045/hybriddb-sys
make test
```

### 3. 运行 4 组论文控制变量基准实验（生成 `results/*.csv`）
```bash
make bench
```

### 4. 使用命令行与交互式工具 `hybriddb_cli`
```bash
# 使用 B+ 树引擎写入、查询与检查内部页结构
./build/hybriddb_cli --engine btree --db ./my_btree put user:1001 "Alice"
./build/hybriddb_cli --engine btree --db ./my_btree get user:1001
./build/hybriddb_cli --engine btree --db ./my_btree inspect

# 使用 LSM-Tree 引擎写入、触发 Compaction 与检查各层 Bloom Filter 分配
./build/hybriddb_cli --engine lsm --db ./my_lsm put user:1001 "Alice"
./build/hybriddb_cli --engine lsm --db ./my_lsm compact
./build/hybriddb_cli --engine lsm --db ./my_lsm inspect
```

---

## 核心实验结果速览（实测数据见 `results/*.csv`）

| 实验编号 | 研究问题与理论来源 | 核心对比维度 | 实测关键结论 |
| :--- | :--- | :--- | :--- |
| **实验 1** (`exp1_btree_vs_lsm.csv`) | **O'Neil et al. (1996)**：B+ 树原地页更新 vs LSM-Tree 延迟批量归并的读写放大取舍 | 随机写吞吐、写放大 (WAF)、点查吞吐、范围扫描吞吐、读放大 (RAF) | **LSM-Tree 随机写吞吐达 `112,577 ops/s`（为 B+ 树的 `2.71x`）**，写放大 `WAF=4.12`（显著低于 B+ 树的 `11.26`）；而 **B+ 树点查快 `1.70x`、范围扫描快 `8.39x`**（`RAF=0.66` vs LSM 的 `5.72`）。 |

> **2026-10-25 性能优化增补**：基于 YCSB 风格基准的瓶颈定位（位级 CRC32 逐页校验 33.87µs、LSM 点查每 SST 每次 open()），实施两项针对性优化——`crc32_ieee` 改 slice-by-8 查表（位级等价）、SSTable 句柄 open-once 缓存。**B+ 树各负载吞吐 5.5–10.6x、LSM 点查最高 61.5x**（对照组 pybitcask/SQLite 漂移 <21%，归因成立），完整前后对比见 [docs/POST_OPTIMIZATION.md](docs/POST_OPTIMIZATION.md)。
| **实验 2** (`exp2_buffer_pool_policies.csv`) | **O'Neil et al. (SIGMOD '93)**：顺序扫描污染（Scan Pollution）下的缓冲池替换策略 | `LRU-1` vs `Clock` vs `LRU-2` 在不同缓冲池帧数 B ∈ {16..64} 下的命中率 | 在 B=24 帧时，**`LRU-2` 命中率达 `61.25%`**，远超 `LRU-1` (`39.59%`) 与 `Clock` (`37.97%`)；`LRU-1` 需将内存扩大至 B=48 帧 (`64.24%`) 才能追平 B=24 的 `LRU-2`（等效内存比 B(1)/B(2) ≈ 2.0）。 |
| **实验 3** (`exp3_monkey_bloom_tuning.csv`) | **Dayan et al. (SIGMOD '17)**：LSM-Tree 跨层级 Bloom Filter 比特分配优化 | 相同总内存预算下 `Uniform`（均匀分配）vs `Monkey`（浅层高比特、深层低比特） | 在 4.0–10.0 bits/key 全部预算下，**`Monkey` 将零结果点查的无效磁盘页读取降低了 `23.0% ~ 38.8%`**（如 4 bpk 下从 `2,506` 次降至 `1,534` 次，分配为 `L0=8.95, L1=6.06, L2=3.18`）。 |
| **实验 4** (`exp4_wal_crash_recovery.csv`) | **Mohan et al. (1992)**：WAL 预写日志崩溃一致性与尾记录自愈 | 缓冲池未刷盘断电（200/1000 ops）与 WAL 末尾 13B 截断损坏注入 | 4 种故障场景下数据恢复准确率均为 **`100% (PASS)`**，1,000 条未刷盘事务在 **`1.25 ms`** 内完成重放恢复，损坏尾字节被精准截断。 |
