# 阶段五至阶段七：系统架构设计、论文实验验证、学期计划与答辩指南

---

## 阶段五：从核心原理开始的 5 层渐进式系统架构设计

我们在 `/mnt/c/Users/18045/hybriddb-sys` 中严格按照五层架构自底向上实现了 `HybridDB`：

```text
+-----------------------------------------------------------------------------------+
| Layer 5: CLI, REPL & Binary Structure Inspector (src/cli_main.cpp)                |
|   - Commands: put, get, del, scan, compact, inspect (page fill factor / LSM bpk)  |
+-----------------------------------------+-----------------------------------------+
                                          |
                  +-----------------------+-----------------------+
                  v                                               v
+---------------------------------------+   +---------------------------------------+
| Layer 4A: Page-Backed B+ Tree Engine  |   | Layer 4B: Multi-Level LSM-Tree Engine |
|   (include/hybriddb/btree.hpp)        |   |   (include/hybriddb/lsm.hpp)          |
| - Internal & Leaf 4KB SlottedPages    |   | - SkipListMemTable (O(log N), L=12)   |
| - Recursive Node Split & Root Growth  |   | - L0..L2 SSTables + Fence Pointers    |
| - Sibling-Linked Leaf Range Scans     |   | - Kirsch-Mitzenmacher Bloom Filter    |
| - Page Fill-Factor & Height Inspector |   | - SIGMOD '17 Monkey Optimal Bit Tuner |
+-------------------+-------------------+   +-------------------+-------------------+
                    |                                           |
                    +---------------------+---------------------+
                                          |
                                          v
+-----------------------------------------------------------------------------------+
| Layer 3: Write-Ahead Log & Crash Recovery (include/hybriddb/wal.hpp)              |
|   - 19-Byte Binary Header (CRC32 + 64-bit LSN + OpType + KeySize + ValSize)       |
|   - Checkpoint LSN Tracking & Idempotent Redo Replay on Startup                   |
|   - Torn-Write Tail Truncation (ftruncate) & Fault Injection (Bit-Flip / Crash)   |
+-----------------------------------------------------------------------------------+
| Layer 2: Buffer Pool Manager & Pluggable Replacers (include/hybriddb/buffer_pool) |
|   - Fixed-Capacity 4KB Frame Pool, Page Table, Pin-Count & Dirty-Bit Tracking     |
|   - Pluggable Policies: Classical LRU-1 | Clock (Second-Chance) | SIGMOD'93 LRU-2 |
+-----------------------------------------------------------------------------------+
| Layer 1: 4KB Slotted-Page & POSIX Disk Manager (include/hybriddb/page.hpp)        |
|   - 4096B SlottedPage: 32B PageHeader (CRC32 + page_lsn) + Forward Slot Directory |
|     + Backward Payload Heap + In-Page Compaction + Binary Search Slot Lookup      |
|   - DiskManager: Thread-Safe POSIX pread / pwrite / fsync + WAF / RAF Telemetry   |
+-----------------------------------------------------------------------------------+
```

### 关键二进制格式规范

#### 1. 4096 字节分槽页（`SlottedPage` in `include/hybriddb/page.hpp`）
```text
Byte 0                                                                      Byte 4095
+--------------------+---------+---------+----------------+---------+---------+
| PageHeader (32B)   | Slot[0] | Slot[1] | ... Free Space | Cell[1] | Cell[0] |
| crc32(4B)          | (8B)    | (8B)    |  -----> <----- | (K1+V1) | (K0+V0) |
| page_lsn(8B)       |         |         |                |         |         |
| page_id(4B)        |         |         |                |         |         |
| page_type(2B)      |         |         |                |         |         |
| num_slots(2B)      |         |         |                |         |         |
| free_space_end(2B) |         |         |                |         |         |
| next/prev_pid(8B)  |         |         |                |         |         |
+--------------------+---------+---------+----------------+---------+---------+
```
- **为什么采用分槽页（Slotted-Page）？**
  变长键值对如果直接顺序紧挨存放，删除或更新变长值会导致整页数据频繁搬移。分槽页将定长 8 字节的 `SlotEntry {offset, key_size, val_size, flags}` 放在页首向右生长，变长 `(key, value)` 放在页尾向左生长。查找时只需对定长 `SlotEntry` 数组做二分查找（$O(\log S)$），页内删除或更新只需修改 8 字节槽位并在空间不足时触发一次页内碎片整理 `compact_payload_if_helpful()`。

#### 2. 预写日志记录格式（`WALRecordHeader` in `include/hybriddb/wal.hpp`）
```text
+------------+----------+-------------+---------------+---------------+---------+-----------+
| crc32 (4B) | lsn (8B) | op_type(1B) | key_size (2B) | val_size (4B) |   key   |   value   |
+------------+----------+-------------+---------------+---------------+---------+-----------+
|<------------------------- 19 Bytes Header ------------------------->|<-- Variable Size -->|
```

---

## 阶段六：控制变量实验结果、论文验证与工程审查

所有实验代码位于 `benchmarks/bench_experiments.cpp`，运行 `make bench` 即可复现并生成 `results/*.csv`。

### 实验 1：B+ 树 vs LSM-Tree 读写放大与吞吐量对比（`results/exp1_btree_vs_lsm.csv`）

在 3,000 个键、128B 值的相同数据集上实测：

| 存储引擎 | 负载类型 | 耗时 (ms) | 吞吐量 (ops/sec) | 写放大 (WAF) | 读放大 (RAF) | 磁盘占用 (Bytes) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **BPlusTree** | 随机写 (`RandomWrite`) | `70.16` | `42,761.66` | **`11.26`** | - | `675,840` |
| **LSMTree** | 随机写 (`RandomWrite`) | **`30.24`** | **`99,194.88` (2.32x)** | **`4.12`** | - | **`462,848`** |
| **BPlusTree** | 随机点查 (`RandomPointRead`) | **`60.79`** | **`49,347.39` (1.97x)** | `11.26` | **`0.61`** | `675,840` |
| **LSMTree** | 随机点查 (`RandomPointRead`) | `119.68` | `25,066.53` | `4.12` | `1.01` | `462,848` |
| **BPlusTree** | 50 键范围扫描 (`RangeScan`) | **`5.15`** | **`38,816.48` (9.53x)** | `11.26` | **`0.66`** | `675,840` |
| **LSMTree** | 50 键范围扫描 (`RangeScan`) | `49.11` | `4,072.90` | `4.12` | `5.72` | `462,848` |

**实验结论（验证 O'Neil et al. 1996 核心定论）**：
- **写入端**：B+ 树在随机写入时因频繁修改不同叶子页并触发节点分裂，每次刷盘都需要写回整个 4096B 页面，导致写放大高达 `11.26`，磁盘由于页内预留空间（平均填充率约 68%）占用 `675 KB`；而 LSM-Tree 将随机写转化为 SkipList 内存插入与紧凑 SSTable 顺序落盘，写放大降至 `4.12`，写入吞吐提升 **2.32 倍**，磁盘空间节省 **31.5%**。
- **读取端**：B+ 树每个键在全局只有唯一叶子位置，且叶子节点通过 `next_page_id` 链表顺序串联，范围扫描吞吐高达 `38,816 scans/s`（读放大仅 `0.66` 页/次）；而 LSM-Tree 范围扫描必须同时归并读取 `MemTable` 与多层 `SSTable` 的数据块（读放大 `5.72` 页/次），因此 B+ 树在范围扫描上领先 **9.53 倍**。

---

### 实验 2：复现 O'Neil et al. (SIGMOD '93) — `LRU-1` vs `Clock` vs `LRU-2` 抗扫描污染实验（`results/exp2_buffer_pool_policies.csv`）

在 256 个磁盘页（前 24 页为接收 85% OLTP 请求的热点页，每 50 次请求穿插一次 12 页的顺序扫描突发）下，改变缓冲池容量 $B \in \{16, 24, 32, 48, 64\}$ 帧：

| 缓冲池容量 $B$ (帧) | `LRU-1` 命中率 | `CLOCK` 命中率 | **`LRU-2` 命中率 (SIGMOD '93)** | `LRU-2` 相比 `LRU-1` 绝对提升 |
| :--- | :--- | :--- | :--- | :--- |
| **16 帧** | `30.10%` | `29.51%` | **`42.60%`** | **`+12.50%` (相对提升 41.5%)** |
| **24 帧** | `39.59%` | `37.97%` | **`61.25%`** | **`+21.66%` (相对提升 54.7%)** |
| **32 帧** | `47.46%` | `45.96%` | **`67.79%`** | **`+20.33%` (相对提升 42.8%)** |
| **48 帧** | `64.24%` | `59.88%` | **`69.51%`** | `+5.27%` |
| **64 帧** | `70.02%` | `67.30%` | **`70.70%`** | `+0.68%` (大容量下收敛) |

**实验结论**：完美复现了 SIGMOD '93 原论文 Table 4.3 的核心规律！当缓冲池容量 $B=24$（刚好足以容纳 24 个热点页）时，传统 `LRU-1` 和 `Clock` 每逢顺序扫描突发就会把热点页大批冲刷出内存，命中率跌至 `39.59%`；而 `LRU-2` 因为顺序扫描页仅被访问 1 次（Backward 2-distance $= \infty$），始终优先淘汰扫描页从而保护了已访问 $\ge 2$ 次的热点页，命中率高达 **`61.25%`**。`LRU-1` 必须把物理内存翻倍到 $B=48$ 帧（`64.24%`）才能达到 $B=24$ 帧下 `LRU-2` 的效果！

---

### 实验 3：复现 Dayan et al. (SIGMOD '17) — `Uniform` vs `Monkey` 布隆过滤器分配实验（`results/exp3_monkey_bloom_tuning.csv`）

在 3 层 LSM-Tree（$N_0=150, N_1=600, N_2=2400$，严格 $1:4:16$ 容量比，均覆盖全键域）上执行 5,000 次零结果点查：

| 平均比特预算 | 分配策略 | 过滤器总内存 (Bytes) | $L_0 / L_1 / L_2$ 分配 (bits/key) | 5,000 次查询无效磁盘页读取数 | 无效磁盘 I/O 削减比例 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **4.00 bpk** | `Uniform` | `1,580 B` | `4.00 / 4.00 / 4.00` | `2,506` 次 (`0.5012` I/O/q) | - |
| **4.00 bpk** | **`Monkey`** | **`1,578 B`** | **`8.95 / 6.06 / 3.18`** | **`1,534` 次 (`0.3068` I/O/q)** | **`-38.79%`** |
| **5.00 bpk** | `Uniform` | `1,973 B` | `5.00 / 5.00 / 5.00` | `1,523` 次 (`0.3046` I/O/q) | - |
| **5.00 bpk** | **`Monkey`** | **`1,971 B`** | **`9.95 / 7.06 / 4.18`** | **`1,113` 次 (`0.2226` I/O/q)** | **`-26.92%`** |
| **6.00 bpk** | `Uniform` | `2,366 B` | `6.00 / 6.00 / 6.00` | `963` 次 (`0.1926` I/O/q) | - |
| **6.00 bpk** | **`Monkey`** | **`2,365 B`** | **`10.95 / 8.06 / 5.18`** | **`742` 次 (`0.1484` I/O/q)** | **`-22.95%`** |
| **8.00 bpk** | `Uniform` | `3,152 B` | `8.00 / 8.00 / 8.00` | `472` 次 (`0.0944` I/O/q) | - |
| **8.00 bpk** | **`Monkey`** | **`3,152 B`** | **`12.95 / 10.06 / 7.18`** | **`364` 次 (`0.0728` I/O/q)** | **`-22.88%`** |
| **10.00 bpk** | `Uniform` | `3,938 B` | `10.00 / 10.00 / 10.00` | `314` 次 (`0.0628` I/O/q) | - |
| **10.00 bpk** | **`Monkey`** | **`3,940 B`** | **`14.95 / 12.06 / 9.18`** | **`241` 次 (`0.0482` I/O/q)** | **`-23.25%`** |

**实验结论**：在完全相同的内存字节消耗下，`Monkey` 从最底层 $L_2$ 仅抽取不到 1 bit/key 的内存补贴给浅层 $L_0$ 和 $L_1$（使 $L_0$ 获得近乎零假阳性的 `8.95 ~ 14.95 bpk`），一举消除了浅层的高频假阳性穿透，将总无效磁盘读取降低了 **23% ~ 38.8%**！

---

### 功能状态分类与审查说明

| 模块与特性 | 当前状态 | 验证方式 |
| :--- | :--- | :--- |
| 4KB `SlottedPage` 变长槽目录、页内碎片整理、CRC32 校验 | **已实现并验证** | `test_slotted_page_and_crc32` |
| `BufferPoolManager` + `LRU-1` / `Clock` / `LRU-2` 替换器 | **已实现并验证** | `test_buffer_pool_lru2_scan_resistance` + 实验 2 |
| `WALManager` 预写日志、断电未刷盘重放、末尾半条记录自动截断 | **已实现并验证** | `test_wal_recovery_and_fault_injection` + 实验 4 |
| `BPlusTreeEngine` 叶子/内部节点递归分裂、范围扫描、WAL 恢复 | **已实现并验证** | `test_bplustree_splits_scans_and_crash_recovery` + 实验 1 |
| `LSMTreeEngine` 跳表、SSTable 稀疏索引、多路归并、Monkey 调优 | **已实现并验证** | `test_lsm_compaction_monkey_and_crash_recovery` + 实验 1/3 |
| 引擎级读写锁（`std::shared_mutex`）多线程安全 | **已实现并验证** | `test_concurrent_multithreaded_engines` |
| B+ 树细粒度页级锁耦合（Crabbing / Latch Coupling） | **尚未实现（后续扩展）** | 当前采用引擎级 `shared_mutex` 保证正确性，可在第 11~12 周扩展 |
| WiscKey 键值分离 `vLog` 存储层 | **尚未实现（后续扩展）** | 已预留为后续研究问题 RQ-1 |

---

## 阶段七：可继续深入的 3 个研究问题、16 周学期开发计划与答辩指南

### 1. 三个可继续深入探索的研究问题（Research Questions）

1. **RQ-1（WiscKey 键值分离的临界点实证研究）**：
   - *问题*：当 Value 大小从 `32B` 增长到 `4KB` 时，在 `HybridDB` 的 LSM-Tree 中引入键值分离（将 Value 存入独立追加写 `vLog`，LSM 仅存 12B `<vlog_id, offset, size>` 指针）在什么临界尺寸（Crossover Size）下能使随机写 + 小范围扫描的综合吞吐超越标准内联 LSM-Tree？
2. **RQ-2（Dostoevsky / 混合 Tiering-Leveling 归并策略研究）**：
   - *问题*：参考 Dayan & Idreos (SIGMOD '18 *Dostoevsky*)，如果将 $L_0 \dots L_{k-1}$ 设为 Tiered（允许同层多文件并存以降低写放大），仅将最底层 $L_k$ 设为 Leveled（单文件有序以保障范围扫描与空间放大），并配合 Monkey 的非均匀 Bloom Filter，能否同时取得 Tiering 的高写入速度与 Leveling 的低点查成本？
3. **RQ-3（B+ 树前缀压缩与页内 SIMD 二分查找优化）**：
   - *问题*：在 `SlottedPage` 内部节点与叶子节点中引入键前缀压缩（Prefix Compression，省去公共前缀如 `user:000...`）后，B+ 树单页扇出（Fanout）能提升多少倍？树高降低是否能进一步减少缓冲池未命中率？

---

### 2. 按周划分的 16 周学期开发与研究计划

| 周次 | 阶段目标 | 具体工程与实验任务 | 对应可交付物 |
| :--- | :--- | :--- | :--- |
| **第 1–2 周** | 文献精读与基线剖析 | 精读 O'Neil 1996 (LSM)、SIGMOD '93 (LRU-K)、SIGMOD '17 (Monkey)；剖析 `google/leveldb` 与 `xv6` 日志源码 | 文献综述报告（已完成于 `docs/stage1_to_4...`） |
| **第 3–4 周** | 第一层：分页存储与 I/O | 设计并验证 4096B `SlottedPage` 二进制格式、CRC32 校验与 `DiskManager` (`pread`/`pwrite`) | `page.hpp`, `disk_manager.cpp` 与页级单元测试 |
| **第 5–6 周** | 第二层：缓冲池与替换器 | 实现 `BufferPoolManager` 及 `LRU-1`、`Clock`、`LRU-2`（含历史保留表），完成扫描污染实验 | `buffer_pool.hpp/.cpp` 与 `exp2_buffer_pool_policies.csv` |
| **第 7–8 周** | 第三层：WAL 与崩溃恢复 | 实现 19B 头部 `WALManager`、尾截断自愈与故障注入器（位翻转、断电丢弃脏帧） | `wal.hpp/.cpp` 与 `exp4_wal_crash_recovery.csv` |
| **第 9–10 周** | 第四层上：B+ 树引擎 | 实现基于缓冲池的 B+ 树叶子/内部节点分裂、兄弟指针链表范围扫描与检查点恢复 | `btree.hpp/.cpp` 与树结构检视器 `inspect` |
| **第 11–12 周** | 第四层下：LSM-Tree 与 Monkey | 实现 `SkipListMemTable`、SSTable 稀疏 Fence Pointers、多路归并 Compaction 与 Monkey 分配器 | `lsm.hpp/.cpp`, `bloom.hpp/.cpp` 与 `exp3_monkey...csv` |
| **第 13–14 周** | 第五层：进阶研究扩展 | 从 RQ-1（WiscKey 键值分离）或 RQ-2（Tiered-Leveled 混合归并）中任选一项实现并跑出对比曲线 | 扩展模块代码与第 5 组对比实验 CSV |
| **第 15–16 周** | 论文撰写与实训答辩 | 整理全部 CSV 实验图表、撰写实训结题论文、准备 CLI 现场故障注入演示与答辩 PPT | 结题报告、答辩演示脚本 |

---

### 3. 项目独立贡献总结、技术难点与答辩准备材料

#### (1) 你在答辩中可重点强调的 3 项独立工程与研究贡献
1. **统一基础设施上的 B+ 树与 LSM-Tree 同构公平对比平台**：
   市面上的对比往往拿 C++ 写的 LevelDB 和 C 写的 SQLite/BerkeleyDB 跨系统对比，受到各自编码格式、锁机制和缓存实现的干扰。本项目的 B+ 树与 LSM-Tree **完全共享同一套 4096B `SlottedPage` 磁盘块格式、同一个 CRC32 算法和同一套 `WALManager`**，使得测出的写放大（`11.26` vs `4.12`）和读放大（`0.66` vs `5.72`）纯粹反映两种索引结构本身的数学本质。
2. **SIGMOD '93 `LRU-2` 算法的真实工程落地与扫描污染复现**：
   不仅实现了课本上的 `LRU-1` 和 `Clock`，还实现了带淘汰页历史队列（Retained History）的 `LRU-2`，用实测数据证明了在存在顺序扫描污染时，`LRU-2` 用一半的物理内存（24 帧）就能达到 `LRU-1` 双倍内存（48 帧）的命中率。
3. **SIGMOD '17 `Monkey` 非均匀布隆过滤器调优的闭环复现**：
   将论文中的拉格朗日最优分配公式 $\Delta b = \frac{\ln(N_{i+1}/N_i)}{(\ln 2)^2}$ 实现为 `BloomFilter::compute_monkey_bits_per_key`，在字节级完全相等的内存预算下实测减少了 23%～38.8% 的无效磁盘 I/O。

#### (2) 评委老师最可能提问的 3 个高频答辩问题与标准回答要点
- **问题 1**：“*既然 LSM-Tree 写入比 B+ 树快 2.3 倍、写放大也小得多，为什么 PostgreSQL、MySQL InnoDB 依然坚持用 B+ 树作为主存储引擎？*”
  - **回答要点**：展示我们的 **实验 1 (`exp1_btree_vs_lsm.csv`)** 数据！在 50 键范围扫描下，B+ 树吞吐量是 LSM-Tree 的 **9.53 倍**（读放大 `0.66` vs `5.72`），随机点查也是 B+ 树的 **1.97 倍**。因为 B+ 树保证每个键在全局只有唯一位置，而 LSM-Tree 的查询必须跨越多个层级的 SSTable 进行归并或探测。因此读多写少、强依赖范围扫描的 OLTP 关系型数据库首选 B+ 树，而写密集型的时序、日志、NoSQL 系统（RocksDB、Cassandra）首选 LSM-Tree。
- **问题 2**：“*为什么顺序扫描会摧毁普通 LRU 缓冲池？你们的 `LRU-2` 是怎么在只增加极小开销的前提下解决它的？*”
  - **回答要点**：结合 **CS:APP 第 6 章局部性原理** 与 **实验 2 数据** 回答。普通 `LRU-1` 把“刚被访问过 1 次”等同于“未来还会被高频访问”（只看时间局部性，不看访问频次）。顺序扫描读入大量只用一次的冷页，瞬间占满 MRU 端。`LRU-2` 通过记录倒数第 2 次访问时间戳，将只访问过 1 次的页面距离设为 $+\infty$，淘汰时始终优先淘汰这些“一次客”页面，从而保护了至少访问过 2 次的热点工作集。
- **问题 3**：“*如果数据库正在写 WAL 日志时突然断电，日志文件的最后一条记录只写了一半，重启时怎么保证既不丢前面已提交的数据，又不把半条乱码当成合法数据？*”
  - **回答要点**：结合 **实验 4 (`exp4_wal_crash_recovery.csv`)** 与 `src/wal.cpp` 回答。我们的每条 WAL 记录头部包含 4 字节 IEEE CRC32 校验和与精确的 `key_size`/`val_size`。重启扫描时，如果文件末尾剩余字节不足 `sizeof(WALRecordHeader)`（19B）、或不足 `19 + key_size + val_size`，或者最后一条记录 CRC32 校验不匹配，引擎识别出这是断电导致的 **Torn Write（撕裂写）**，自动调用 `ftruncate()` 将文件精准截断回上一条合法记录的末尾偏移量，并重放前面所有校验通过的已提交记录。
