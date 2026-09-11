# UDB 开发简报

## 阶段 0：C++17 工程基线

- 做了什么：建立 C++17 + CMake 最小工程。
- 关键设计：C++17、关闭编译器扩展、开启常用警告，无第三方依赖。
- 测试结果：CMake 配置、编译和运行成功，无警告。
- Commit：da1fe6ac7a6709c9d597b56136febdd2c3714daf
- 下一步：Page + DiskManager。

## 阶段 1：Page + DiskManager

- 做了什么：实现 4096 字节 Page、page_id_t 和 DiskManager，支持页面分配、整页读写和重新打开后的数据读取。
- 关键设计：page_id_t 为 std::int64_t，从 0 递增；磁盘偏移为 page_id * PAGE_SIZE；新页面填零，只允许访问已分配页面。
- 测试结果：构建成功，无警告；页面分配、读写、重开持久性和异常边界测试全部通过。
- Commit：90e0a86901711e5926a0f24519ddcf28fedf81e9
- 下一步：Buffer Pool。

## 阶段 2：Buffer Pool

- 做了什么：实现固定容量 BufferPoolManager，支持 Fetch、New、Unpin 和显式刷盘。
- 关键设计：固定 frame + 页映射；按最近访问顺序执行 LRU，仅淘汰未 pin 页面；脏页淘汰前写回，析构不自动刷盘。
- 测试结果：从零 CMake 构建成功，无警告；全部存储与 Buffer Pool 测试通过，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：评审上层存储需求，确定下一阶段范围。

## 阶段 3：Record + Slotted Page

- 做了什么：实现二进制 Record、可比较 RID，以及页内变长记录插入、读取、删除压缩和 next_page_id。
- 关键设计：显式小端布局与边界校验；删除压缩保持其他 RID 稳定；不复用删除槽以防旧 RID 指向新记录，槽目录空间保留。
- 测试结果：从零构建无警告，全部测试及 ASan/UBSan 检查通过，git diff --check 通过。
- 下一步：TableHeap。

## 阶段 4：TableHeap

- 做了什么：实现多页表的创建、重开、插入、读取、删除与顺序扫描。
- 关键设计：仅通过 Buffer Pool 访问页面，局部 RAII 释放 pin；递增页链仅在链尾扩展，支持单 frame；调用方保存 first_page_id 并显式刷盘。
- 测试结果：从零构建无警告，全部 4 个测试及 ASan/UBSan 通过，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：基础类型系统、Schema 与 Tuple。

## 阶段 5：Type + Schema + Tuple

- 做了什么：实现四种基础类型、Value、Column、Schema 与 Tuple，以及 Tuple/Record 转换和存储集成。
- 关键设计：严格类型与带类型 NULL；VARCHAR 按字节限长；显式小端编码并校验边界，TableHeap 保持仅存 Record。
- 测试结果：从零构建无警告，全部 5 个测试及 ASan/UBSan 通过，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：Catalog 与表元数据。

## 阶段 6：Catalog + TableMetadata

- 做了什么：实现内存 Catalog，管理表元数据和 TableHeap，支持创建、按 ID/名称查询与列举。
- 关键设计：uint64_t 表 ID 单调递增，表名区分大小写且唯一；完整创建后登记，统一拥有元数据与表；不持久化 Catalog、不自动刷盘。
- 测试结果：从零构建无警告，全部 6 个测试及 ASan/UBSan 通过，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：Catalog 持久化与数据库元数据。

## 阶段 7：Catalog Persistence + Database

- 做了什么：实现 Database 创建、打开、显式 Flush/Close，以及 Catalog、Schema 和表链恢复。
- 关键设计：独立 .meta 使用版本化小端编码并严格校验；先刷数据再通过 .meta.tmp 替换元数据；Open 复用已有页并恢复 next_table_id。独立 .meta 是当前 bootstrap 设计，未来可能演进为统一数据库文件。
- 测试结果：从零构建无警告，全部 7 个测试及 ASan/UBSan 通过，重开恢复、损坏元数据和保存失败重试测试通过，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：SQL Parser 基础。

## 阶段 8：SQL Lexer + Parser + AST

- 做了什么：实现独立 Lexer、Token、手写 Parser 和 CREATE TABLE / INSERT / SELECT AST。
- 关键设计：关键字忽略大小写、标识符保留原文；字符串支持 SQL 单引号转义，错误含行列位置；单语句必须消费到 EOF，不访问 Catalog、不执行 SQL。
- 测试结果：从零构建及检测版重编译无警告，全部 8 个测试及 ASan/UBSan 通过，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：Binder。

## 阶段 9：Binder

- 做了什么：实现 CREATE TABLE、INSERT、SELECT 语义绑定与独立 Bound Statement。
- 关键设计：只读 Catalog，绑定结果拥有 Schema/Value 副本；整数 literal 按 int32 范围区分 INTEGER/BIGINT，严格匹配且 NULL 使用目标类型；投影使用列下标，沿用 Schema 唯一列名约束，暂拒绝重复投影。
- 测试结果：从零构建无警告，全部 9 个测试及 ASan/UBSan 通过，git diff --check 通过。
- 下一步：Logical Plan / Planner。

## 阶段 10：Logical Plan + Planner

- 做了什么：实现独立 PlanNode、CreateTablePlan、InsertPlan、SeqScanPlan 与 Planner。
- 关键设计：直接复制 Bound 信息，不重复绑定或访问 Catalog；当前均为叶节点，CREATE/INSERT 无输出，扫描输出保持投影顺序；计划独立拥有 Schema 与 Value。
- 测试结果：从零构建及检测版重编译无警告，全部 10 个测试及 ASan/UBSan 通过，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：Executor。

## 阶段 11：Executor

- 做了什么：实现 CREATE TABLE、INSERT 与顺序扫描执行，返回执行结果和投影后的 Tuple。
- 关键设计：直接使用 Plan 中的表 ID 和列下标；通过 Catalog / TableHeap 访问数据；保持 Database 显式 Flush / Close 语义。
- 测试结果：从零构建无警告，全部 11 个测试及 ASan/UBSan 通过，覆盖完整 SQL 链路、多页扫描和重开恢复，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：SQL Engine / Database 对外执行接口。

## 阶段 12：SQL Engine

- 做了什么：实现统一 ExecuteSQL 入口，串联 Parser、Binder、Planner 与 Executor。
- 关键设计：SqlEngine 仅持有 Catalog 引用并编排现有模块；复用 ExecutionResult；异常原样传播且不改变显式 Flush / Close 语义。
- 测试结果：从零构建无警告，全部 12 个测试及 ASan/UBSan 通过，覆盖错误传播、多页查询和重开执行，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：WHERE 与表达式系统。

## 阶段 13：Expression + WHERE

- 做了什么：实现 AST / Bound Expression、比较与逻辑运算，并为 SELECT 加入 WHERE 过滤。
- 关键设计：Bound 列引用只保存列下标；SeqScanPlan 携带可选 predicate；比较和 AND / OR / NOT 遵循 SQL 三值逻辑。
- 测试结果：从零构建无警告，全部 13 个测试及 ASan/UBSan 通过，覆盖优先级、类型绑定、多页过滤和重开查询，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：DELETE。

## 阶段 14：DELETE

- 做了什么：实现 DELETE AST、绑定、计划与执行，支持可选 WHERE 和准确 affected rows。
- 关键设计：完全复用 Bound Expression 与三值逻辑；先收集匹配 RID 再通过 TableHeap 删除；保持显式 Flush / Close 持久化语义。
- 测试结果：从零构建无编译警告，全部 14 个测试及 ASan/UBSan 通过，覆盖跨页删除、RID 稳定性和重开恢复，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：UPDATE。

## 阶段 15：UPDATE

- 做了什么：实现 SlottedPage / TableHeap 原位 Record 更新，以及 UPDATE 的解析、绑定、计划与执行。
- 关键设计：临时 Page 重排保证单条失败不修改原记录且 RID 稳定；SET 绑定为列下标和已类型化 Value；先收集替换内容再更新。
- 测试结果：从零构建无警告，全部 15 个测试及 ASan/UBSan 通过，覆盖跨页更新、空间失败原子性和重开恢复，git diff --check 通过。
- 当前限制：增长后的 Record 若当前 Page 放不下则更新失败；无事务时不保证多行 UPDATE 原子性。
- Commit：见 Git 历史
- 下一步：DROP TABLE 与基础 DDL 完善。

## 阶段 16：DROP TABLE

- 做了什么：实现 Catalog 表删除以及 DROP TABLE 的解析、绑定、计划与执行。
- 关键设计：Catalog 删除其持有的 TableMetadata 与 TableHeap；table_id 保持单调递增，同名重建获得新 ID；持久化元数据只保存当前表。
- 测试结果：从零构建无警告，全部 16 个测试及 ASan/UBSan 通过，覆盖同名重建、表间隔离和重开恢复，git diff --check 通过。
- 当前限制：DROP 后暂不回收数据页。
- Commit：见 Git 历史
- 下一步：Page 回收与 Free Page 管理。

## 阶段 17：Free Page Management

- 做了什么：实现 Page 释放与复用，并让 DROP TABLE 回收完整 TableHeap 页面链。
- 关键设计：DiskManager 确定性复用最小空闲页并清零；BufferPoolManager 拒绝删除 pinned 页且清除缓存状态；metadata v2 以小端格式持久化空闲页集合。
- 测试结果：从零构建无警告，全部 17 个测试及 ASan/UBSan 通过，覆盖跨页回收、重开复用和损坏元数据，git diff --check 通过。
- 当前限制：释放 Page 会复用，但不会缩小 .udb 文件。
- Commit：见 Git 历史
- 下一步：B+ Tree 索引基础。

## 阶段 18：B+ Tree 基础

- 做了什么：实现持久化 int64_t 到 RID 的 B+ Tree，支持精确查询、插入及 Leaf/Internal/Root split。
- 关键设计：节点采用固定宽度小端页面格式；Internal separator 表示右侧 child 的最小 key；所有页面通过 BufferPoolManager 和局部 pin guard 访问。
- 测试结果：从零构建无警告，全部 18 个测试及 ASan/UBSan 通过，覆盖 4000 个乱序 key、小 Buffer Pool、多层 split、重开与继续插入，git diff --check 通过。
- 当前限制：仅支持 int64_t key，不支持删除。
- Commit：见 Git 历史
- 下一步：B+ Tree Delete / Merge / Redistribution。

## 阶段 19：Index Metadata + CREATE INDEX

- 做了什么：实现 Catalog 持久化 IndexMetadata 和 CREATE INDEX 全链路，并从现有 TableHeap 数据构建索引。
- 关键设计：索引以稳定 B+ Tree header_page_id 标识；CREATE INDEX 先完整构建、后注册；INTEGER / BIGINT 统一为 int64_t key，跳过 NULL，当前仅支持单列唯一索引；metadata v3 兼容 v1 / v2。
- 测试结果：从零构建无警告，全部 19 个测试及 ASan / UBSan 通过，覆盖多层 split、重开、DROP 清理和损坏 metadata，git diff --check 通过。
- 当前限制：INSERT / UPDATE / DELETE 尚不维护索引，SELECT 尚不使用索引；无 DROP INDEX、非唯一索引和 B+ Tree Delete；失败创建索引或 DROP TABLE 后索引页面暂不回收。
- Commit：见 Git 历史
- 下一步：阶段 20：DML Index Maintenance。

## 阶段 20：B+ Tree Delete + DML Index Maintenance

- 做了什么：实现 B+ Tree 删除，并让 INSERT / UPDATE / DELETE 自动维护表上的全部索引。
- 关键设计：借位或合并后重建 separator，合并页立即回收且 root collapse 同步 header；DML 先检查 unique 冲突，NULL 不进入索引，UPDATE 只处理实际变化的 indexed column。
- 测试结果：从零构建无编译警告，20 / 20 测试及 ASan / UBSan 通过，覆盖多层删除、capacity=1、DML 冲突一致性和重开恢复，git diff --check 通过。
- 当前限制：SELECT 仍使用 SeqScan；无 DROP INDEX、非唯一索引、事务、WAL 与 MVCC。
- Commit：见 Git 历史
- 下一步：阶段 21：Index Scan + Planner 索引选择。

## 阶段 21：Index Scan + Planner 索引选择

- 做了什么：新增 IndexScanPlan，并让 SQL Planner 为 INTEGER / BIGINT 单列索引等值条件选择 B+ Tree 精确查询。
- 关键设计：支持 column = literal 与反向形式；AND 可提取索引 key 但保留完整 predicate 作为 residual filter；其他条件继续 SeqScan，多索引按最小 index_id 确定性选择。
- 测试结果：从零构建无编译警告，21 / 21 测试及 ASan / UBSan 通过，覆盖多页表、DML 后查询、重开和 SeqScan 语义对照，git diff --check 通过。
- 当前限制：仅支持精确等值 IndexScan，无范围扫描、成本模型、复合索引和 index-only scan。
- Commit：见 Git 历史
- 下一步：阶段 22：B+ Tree Range Scan + 范围条件索引查询。

## 阶段 22：B+ Tree Range Scan + 范围索引查询

- 做了什么：实现 B+ Tree 有界与单边范围扫描，并让 Planner 为 INTEGER / BIGINT 范围条件生成 IndexRangeScanPlan。
- 关键设计：通过 leaf chain 按 key 顺序扫描；支持正反向比较与 AND 上下界合并；完整 predicate 继续作为 residual filter，NULL、!= 与 OR 回退 SeqScan。
- 测试结果：从零构建无编译警告，22 / 22 测试及 ASan / UBSan 通过，覆盖开闭边界、跨页、删除、DML、重开和 SeqScan 对照，git diff --check 通过。
- 当前限制：无 ORDER BY 保证、成本模型、复合索引和 index-only scan。
- Commit：见 Git 历史
- 下一步：阶段 23：DROP INDEX 与索引页面回收。
