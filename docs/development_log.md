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

## 阶段 23：DROP INDEX + 索引页面回收

- 做了什么：实现 Catalog 与 SQL DROP INDEX，并让 DROP INDEX / DROP TABLE 回收 B+ Tree 全部节点页和 header page。
- 关键设计：删除前完整验证页面集合并检查 pin 状态；索引 ID 保持单调且名称可复用；metadata 格式不变，Flush 仅保存当前索引。
- 测试结果：从零构建无编译警告，23 / 23 测试及 ASan / UBSan 通过，覆盖多层树回收、拒绝 pinned page、Planner 回退、重开和页面复用，git diff --check 通过。
- 当前限制：无 IF EXISTS、CASCADE/RESTRICT 和并发 DDL。
- Commit：见 Git 历史
- 下一步：阶段 24：SELECT LIMIT。

## 阶段 24：SELECT LIMIT

- 做了什么：为 SELECT 增加非负整数字面量 LIMIT，并贯通 Parser、Binder、三类扫描 Plan 与 Executor。
- 关键设计：LIMIT 在完整 predicate 过滤后计数；LIMIT 0 直接返回；SeqScan、IndexScan 和 IndexRangeScan 共享一致语义。
- 测试结果：从零构建无编译警告，24 / 24 测试及 ASan / UBSan 通过，覆盖错误语法、空表、DML、索引扫描和重开，git diff --check 通过。
- 当前限制：无 OFFSET、参数化 LIMIT 和 ORDER BY。
- Commit：见 Git 历史
- 下一步：阶段 25：SELECT ORDER BY。

## 阶段 25：SELECT ORDER BY

- 做了什么：为 SELECT 增加单列 ORDER BY ASC / DESC，并贯通 Parser、Binder、三类扫描 Plan 与 Executor。
- 关键设计：采用稳定内存排序；ASC / DESC 均将 NULL 放在末尾；完整 predicate 过滤后排序，最后应用 LIMIT。
- 测试结果：全新构建无编译警告，25 / 25 测试及 ASan / UBSan 通过，覆盖类型排序、稳定性、索引扫描、LIMIT 和重开，git diff --check 通过。
- 当前限制：无多列排序、表达式排序和外部排序。
- Commit：见 Git 历史
- 下一步：阶段 26：ORDER BY 多列排序。

## 阶段 26：ORDER BY 多列排序

- 做了什么：将 SELECT ORDER BY 扩展为任意多个列，每列可独立指定 ASC / DESC。
- 关键设计：按声明顺序逐列比较；每个排序键均采用 NULLS LAST；完全相同的排序键保持输入顺序，LIMIT 仍在排序后应用。
- 测试结果：全新构建无编译警告，26 / 26 测试及 ASan / UBSan 通过，覆盖混合方向、稳定性、索引路径、LIMIT 和重开，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 27：OFFSET。

## 阶段 27：OFFSET

- 做了什么：为 SELECT 增加 `LIMIT n OFFSET m`，并贯通 Binder、三类扫描 Plan 与 Executor。
- 关键设计：OFFSET 在过滤和排序后跳过结果，再应用 LIMIT；仅允许与 LIMIT 组合；三种扫描路径共享相同分页逻辑。
- 测试结果：全新构建无编译警告，27 / 27 测试及 ASan / UBSan 通过，覆盖语法边界、排序、索引路径、超范围和重开，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 28：无 GROUP BY 的基础 Aggregate。

## 阶段 28：基础 Aggregate

- 做了什么：实现无 GROUP BY 的 COUNT、SUM、MIN、MAX 与 AVG，支持 WHERE、NULL、空输入及 LIMIT / OFFSET。
- 关键设计：聚合使用独立 AggregatePlan；COUNT / SUM 输出 BIGINT，AVG 输出 DOUBLE；NULL 不参与列聚合，SUM 检测溢出。
- 测试结果：全新构建无编译警告，28 / 28 测试及 ASan / UBSan 通过，覆盖类型检查、空集、过滤、溢出和重开，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 29：单列 GROUP BY + Aggregate。

## 阶段 29：单列 GROUP BY

- 做了什么：为基础聚合增加单列 GROUP BY，支持输出或隐藏分组键。
- 关键设计：NULL 形成一个独立组；组按首次出现顺序输出；过滤先于分组，LIMIT / OFFSET 后于分组。
- 测试结果：全新构建无编译警告，29 / 29 测试及 ASan / UBSan 通过，覆盖聚合、NULL、过滤、空表、分页和重开，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 30：HAVING。

## 阶段 30：HAVING

- 做了什么：为全局与分组聚合增加 HAVING，支持比较、AND / OR / NOT 和三值逻辑。
- 关键设计：HAVING 只绑定 SELECT 已输出的分组键与聚合项；在聚合后过滤，在 LIMIT / OFFSET 前执行。
- 测试结果：全新构建无编译警告，30 / 30 测试及 ASan / UBSan 通过，覆盖全局/分组聚合、DOUBLE、NULL、分页和重开，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 31：SELECT 表达式与别名。

## 阶段 31：SELECT 表达式与别名

- 做了什么：SELECT 支持列和字面量投影、AS 别名，以及 INTEGER / BIGINT 的 `+ - * /` 基础算术表达式。
- 关键设计：算术遵循乘除优先级与括号；NULL 传播；除零和整数溢出明确报错；表达式投影复用现有 BoundExpression。
- 测试结果：全新构建无编译警告，31 / 31 测试及 ASan / UBSan 通过，覆盖别名、优先级、过滤、索引路径、错误边界和重开，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 32：CROSS JOIN。

## 阶段 32：CROSS JOIN

- 做了什么：支持两张表的 CROSS JOIN，以及限定列名、过滤、投影、排序和分页。
- 关键设计：使用嵌套循环并复用 TableHeap；歧义列名明确报错；保持单 frame 可执行，不改变持久化格式。
- 测试结果：干净构建无警告，32 / 32 测试及 ASan / UBSan 通过；覆盖重开和表达式排序，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 33：INNER JOIN。

## 阶段 33：INNER JOIN

- 做了什么：支持 JOIN / INNER JOIN ... ON 的双表等值连接。
- 关键设计：ON 绑定两表同类型列，独立保留 WHERE；嵌套循环复用 TableHeap，NULL 不匹配，重复键保留全部组合。
- 测试结果：全新构建无警告，33 / 33 测试及 ASan / UBSan 通过，覆盖多页、单 frame、排序分页与重开；git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 34：Hash Join。

## 阶段 34：Hash Join

- 做了什么：增加可显式选择的 Hash Join，支持 INTEGER / BIGINT / BOOLEAN / VARCHAR 等值连接。
- 关键设计：右表建桶、左表探测；NULL 不入桶，重复键保留全部组合和原顺序；复用过滤、投影与分页。
- 测试结果：全新构建无警告，34 / 34 测试及 ASan / UBSan 通过；与 Nested Loop 对照覆盖多页、二进制字符串、重开，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 35：Join Planner。

## 阶段 35：Join Planner

- 做了什么：SQL 规划自动为支持的列等值连接选择 Hash Join。
- 关键设计：只使用 Bound 列索引和类型；CROSS JOIN / DOUBLE 保留 Nested Loop；无 Catalog 规划保留参考执行路径。
- 测试结果：全新构建无警告，35 / 35 测试及 ASan / UBSan 通过，覆盖选择规则、残余过滤与结果一致性，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 36：Executor Operator 抽象。

## 阶段 36：Executor Operator 抽象

- 做了什么：提取扫描、过滤、投影算子并接入 Executor。
- 关键设计：算子拥有子输入，复用 BoundExpression；暂按批次执行，保持排序分页及显式刷盘语义。
- 测试结果：全新构建无警告，36 / 36 测试及 ASan / UBSan 通过，覆盖算子组合、NULL、二进制数据与异常后页面访问，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 37：Iterator Execution。

## 阶段 37：Iterator Execution

- 做了什么：算子提供 Init / Next，无排序顺序查询按行执行过滤、分页与投影。
- 关键设计：Execute 仅收集最终输出；LIMIT 停止拉取输入；扫描不跨调用持有 page pin，支持重置。
- 测试结果：全新构建无警告，37 / 37 测试及 ASan / UBSan 通过，覆盖惰性读取、提前结束、重置和单 frame 交错访问，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 38：Sort / Aggregate / Join 统一 pipeline。

## 阶段 38：Sort / Aggregate / Join 统一 pipeline

- 做了什么：排序、聚合、嵌套循环与 Hash Join 接入统一 Init / Next 算子链。
- 关键设计：Join 按行输出，聚合按行累积分组状态；排序保留稳定物化；统一复用过滤、分页与投影。
- 测试结果：全新构建无警告，38 / 38 测试及 ASan / UBSan 通过，覆盖组合、重置、提前结束及原 SQL 回归，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 39：Non-unique B+ Tree Index。

## 阶段 39：Non-unique B+ Tree Index

- 做了什么：通过 Catalog options 支持非唯一索引，DML、精确与范围扫描维护/返回全部 RID。
- 关键设计：唯一 key 节点结构不变，重复值使用可回收 RID 列表页；header 持久化模式并兼容旧 unique 索引；SQL 建索引默认行为不变。
- 测试结果：全新构建无警告，39 / 39 测试及 ASan / UBSan 通过，覆盖列表跨页、乱序删除、回收、多索引 DML 与重开，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 40：VARCHAR Index。

## 阶段 40：VARCHAR Index

- 做了什么：VARCHAR 支持唯一/非唯一索引、DML 维护、精确和范围查询。
- 关键设计：IndexKey 保留完整字节与长度；字符串 key 上限 1024 字节，页容量随声明长度调整；新 header 兼容旧整数格式。
- 测试结果：全新构建无警告，40 / 40 测试及 ASan / UBSan 通过，覆盖前缀、内嵌零、最大长度、多层分裂、删除与重开，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 41：Composite Index。

## 阶段 41：Composite Index

- 做了什么：支持多列索引 key、Catalog 元数据、DML 维护和完整等值条件匹配。
- 关键设计：按声明列顺序编码；任一 NULL 不入索引；Planner 仅在所有索引列等值绑定时选择复合索引，并优先更完整 key。
- 测试结果：全新构建无警告，41 / 41 测试及 ASan / UBSan 通过，覆盖唯一/非唯一、DML、NULL、重开及 v3 兼容，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 42：Index-only Scan 基础。

## 阶段 42：Index-only Scan 基础

- 做了什么：为单列整数、BIGINT 与 VARCHAR 索引增加覆盖查询计划和执行路径。
- 关键设计：仅在索引完整覆盖投影和谓词时选用；结果直接由索引 key 构造，不读取 TableHeap；精确与范围扫描均保留 NULL、LIMIT 和 OFFSET 语义。
- 测试结果：全新构建无警告，42 / 42 测试及 ASan / UBSan 通过，覆盖唯一/非唯一、范围、别名、重开与无表页读取，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 43：Statistics。

## 阶段 43：Statistics

- 做了什么：Catalog 可显式采集表行数及逐列 NULL、非 NULL、基数、最小值和最大值。
- 关键设计：统计是按需生成的内存快照；DML 后由调用方重新分析；数据库重开后可从 TableHeap 重建，不修改持久化格式。
- 测试结果：全新构建无警告，43 / 43 测试及 ASan / UBSan 通过，覆盖全部现有类型、空表、NULL、刷新和重开重建，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 44：Cost Model 基础。

## 阶段 44：Cost Model 基础

- 做了什么：为 SeqScan、精确索引、范围索引和 Index-only Scan 提供粗粒度成本与结果量估算。
- 关键设计：优先使用 Catalog 统计与索引唯一性；无统计时使用确定性保守默认值；LIMIT / OFFSET 计入索引读取量。
- 测试结果：全新构建无警告，44 / 44 测试及 ASan / UBSan 通过，覆盖选择率、唯一/非唯一、覆盖扫描、分页与无统计回退，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 45：Access Path Optimizer。

## 阶段 45：Access Path Optimizer

- 做了什么：Planner 基于成本比较 SeqScan、精确索引、范围索引与 Index-only Scan 候选。
- 关键设计：枚举所有安全匹配的索引；使用统计选择访问路径；同成本时优先覆盖更多列，并保持索引 ID 顺序稳定。
- 测试结果：全新构建无警告，45 / 45 测试及 ASan / UBSan 通过，覆盖小表、低/高基数、多候选、覆盖扫描与残余过滤，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 46：Join Order 基础。

## 阶段 46：Join Order 基础

- 做了什么：基于表统计为当前双表 Join 选择物理输入顺序。
- 关键设计：Hash Join 在较小输入建表；Nested Loop 以较小输入为外表；输出始终恢复逻辑左右表列顺序。
- 测试结果：全新构建无警告，46 / 46 测试及 ASan / UBSan 通过，覆盖有/无统计、左右侧选择、两种 Join 与结果语义，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：STOP；重新评审存储、Executor 与 Catalog 架构后再制定事务相关 roadmap。

## 阶段 47：Page Guard

- 做了什么：实现统一 ReadPageGuard / WritePageGuard，并替换 TableHeap 与 B+ Tree 的局部 pin/unpin RAII。
- 关键设计：Guard 独占且可移动；Read Guard 仅暴露 const 页面；Write Guard 是统一可写入口并在释放时自动标脏。
- 测试结果：全新构建无警告，47 / 47 测试及 ASan / UBSan 通过，覆盖移动、显式释放、异常清理、dirty 持久化和单 frame 存储回归，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 48：Transaction Core。

## 阶段 48：Transaction Core

- 做了什么：实现 transaction_id、Transaction 状态机、TransactionManager 和 ExecutionContext，并贯通 SqlEngine 到 Executor。
- 关键设计：事务由 Manager 持有且 ID 单调递增；状态只允许 Active 转为 Committed/Aborted；旧无事务执行接口保持兼容。
- 测试结果：全新构建无警告，48 / 48 测试及 ASan / UBSan 通过，覆盖状态转换、归属校验、上下文传播和错误路径，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 49：Explicit Transactions。

## 阶段 49：Explicit Transactions

- 做了什么：实现 BEGIN / COMMIT / ROLLBACK 与 autocommit，并支持单线程事务回滚。
- 关键设计：WritePageGuard 首次写入捕获整页 before-image；页面分配与释放进入事务写集；显式事务拒绝 DDL。
- 测试结果：全新构建无警告，49 / 49 测试及 ASan / UBSan 通过，覆盖 DML、索引、跨页、allocate/free、约束失败与重开持久性，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 50：WAL Foundation。

## 阶段 50：WAL Foundation

- 做了什么：新增数据库伴生 `.wal`、LogManager、LSN 与六类事务/页面日志记录。
- 关键设计：日志采用版本化定长帧头与 CRC32；PAGE_WRITE 保存整页 before/after image；LSN 连续分配并支持 fsync 持久化边界。
- 测试结果：全新构建无警告，50 / 50 测试及 ASan / UBSan 通过，覆盖全类型编解码、重开续写、损坏/截断校验和数据库生命周期，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 51：Write-Ahead Rule。

## 阶段 51：Write-Ahead Rule

- 做了什么：WritePageGuard 接入整页 WAL，页面分配/释放和事务生命周期自动写日志，并强制写前持久化顺序。
- 关键设计：frame 记录最新 LSN；脏页落盘前确保 WAL durable；COMMIT 记录 fsync 后才确认，Guard 析构错误延迟到安全边界传播。
- 测试结果：全新构建无警告，51 / 51 测试及 ASan / UBSan 通过，覆盖 before/after image、淘汰/显式刷页顺序、allocate/free、COMMIT/ABORT 与重开，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 52：Crash Recovery。

## 阶段 52：Crash Recovery

- 做了什么：启动时扫描 WAL，对已提交事务 REDO，并对中止及未完成事务反向 UNDO，同时恢复页面分配状态。
- 关键设计：恢复使用整页 before/after image；截断尾日志安全丢弃；恢复可重复执行，干净关闭后回收 WAL。
- 测试结果：全新构建无警告，52 / 52 测试及 ASan / UBSan 通过，覆盖提交重做、loser 回滚、allocate/free、截断日志与重复恢复，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 53：Checkpoint。

## 阶段 53：Checkpoint

- 做了什么：实现显式 checkpoint，以安全顺序持久化 WAL、数据页和 Catalog 元数据，并回收已完成日志。
- 关键设计：数据文件和原子替换的 metadata 均执行 durable sync；存在活动事务时拒绝 checkpoint；关闭数据库复用同一 checkpoint 路径。
- 测试结果：全新构建无警告，53 / 53 测试及 ASan / UBSan 通过，覆盖 WAL 回收、checkpoint 后尾日志恢复和活动事务屏障，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 54：Thread-safe Storage。

## 阶段 54：Thread-safe Storage

- 做了什么：为 DiskManager、BufferPoolManager 和 Page Guard 增加线程安全的状态保护与页面读写 latch。
- 关键设计：Buffer Pool 元数据由单一 mutex 保护；每个 frame 使用共享/独占 latch；页面保持 pinned 直到 Guard 释放 latch，避免并发淘汰。
- 测试结果：全新构建无警告，54 / 54 测试及 ASan / UBSan 通过，并发分配、读写、pin/fetch/flush 压力测试通过，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 55：Concurrent Table/Index。

## 阶段 55：Concurrent Table/Index

- 做了什么：为 TableHeap、B+ Tree 和 Catalog 增加模块级并发保护，保证页面链、树结构及元数据发布的一致性。
- 关键设计：TableHeap 与 B+ Tree 使用共享/独占锁区分读取和结构修改；Catalog 使用粗粒度递归 mutex 保持现有嵌套接口；锁顺序保持在 Page Guard 之上。
- 测试结果：全新构建无警告，55 / 55 测试及 ASan / UBSan 通过，并发跨页插入、索引分裂/删除/查询和 Catalog 发布压力测试通过，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 56：Lock Manager。

## 阶段 56：Lock Manager

- 做了什么：实现 table/row 级 Shared/Exclusive LockManager、阻塞等待、锁升级与事务持锁集合。
- 关键设计：资源队列采用 FIFO 兼容性授予；同一资源只允许一个 S→X upgrader；transaction_id 在进程内跨 Manager 唯一。
- 测试结果：全新构建无警告，56 / 56 测试及 ASan / UBSan 通过，覆盖兼容矩阵、等待唤醒、升级、独立行资源、UnlockAll 与错误边界，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 57：Strict 2PL。

## 阶段 57：Strict 2PL

- 做了什么：将共享 LockManager 接入 SQL 执行链，SELECT/DML 按事务持锁，并在提交或回滚完成后统一释放。
- 关键设计：第一版采用表级 S/X 锁；多表查询按 table_id 顺序加锁；Buffer Pool 活动事务改为线程局部，COMMIT durable/ROLLBACK 恢复完成后才解锁。
- 测试结果：全新构建无警告，57 / 57 测试及 ASan / UBSan 通过，覆盖可重复读、脏读阻断、提交/回滚唤醒和不同表并发 DML，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 58：Deadlock。

## 阶段 58：Deadlock

- 做了什么：为 LockManager 增加 wait-for graph、环检测与死锁受害事务唤醒中止。
- 关键设计：每次阻塞请求建立跨 table/row 资源依赖；选择环中 transaction_id 最大者；SqlEngine 通过已有 before-image/WAL Abort 路径回滚后释放锁。
- 测试结果：全新构建无警告，58 / 58 测试及 ASan / UBSan 通过，覆盖等待图、确定性 victim、继续执行、数据回滚与 WAL ABORT，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 59：Isolation Levels。

## 阶段 59：Isolation Levels

- 做了什么：加入 READ COMMITTED 与 REPEATABLE READ 事务隔离级别，并让 SQL Engine 支持选择默认或 BEGIN 指定级别。
- 关键设计：REPEATABLE READ 保留读锁至事务结束；READ COMMITTED 在只读语句结束释放新取得的 S 锁；两者的 X 锁都遵循 Strict 2PL。
- 测试结果：全新构建无警告，59 / 59 测试及 ASan / UBSan 通过，覆盖 dirty read 阻断、non-repeatable read、repeatable read 和写锁保持，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 60：Concurrent Recovery Stress。

## 阶段 60：Concurrent Recovery Stress

- 做了什么：补充多 session 事务、TableHeap、B+ Tree、WAL 与 crash/reopen 综合压力测试，并修正并发事务的恢复重放顺序。
- 关键设计：按事务 COMMIT/ABORT 的 WAL 位置重放已结束事务，活跃 loser 最后逆序 UNDO；以 Strict 2PL 的事务结束顺序保持冲突写入的最终次序。
- 测试结果：全新构建无警告，60 / 60 测试及 ASan / UBSan 通过，综合场景连续重复 20 次通过，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：STOP；按 roadmap 停止，等待 MVCC 前架构评审。

## 阶段 61：Same-page Transaction Safety

- 做了什么：为事务写入增加 page lifetime ownership，防止同一物理页上不同 RID 的整页回滚互相覆盖。
- 关键设计：写页、新页和删页由首个事务持有至 COMMIT durable 或 ROLLBACK 完成；后继写者等待所有权释放；RID 与 Record/Tuple 格式不变。
- 测试结果：全新构建无警告，61 / 61 测试及 ASan / UBSan 通过，同页 abort/commit/crash 场景连续重复 20 次通过，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 62：MVCC Timestamp Core。

## 阶段 62：MVCC Timestamp Core

- 做了什么：为 Transaction 增加 read_ts / commit_ts，并实现进程级 commit timestamp 与 active snapshot watermark。
- 关键设计：BEGIN 捕获最近已发布提交时间；COMMIT 在 WAL durable 后串行分配时间戳；watermark 跟踪最老活动 read_ts，Manager 销毁时清理遗留登记。
- 测试结果：全新构建无警告，62 / 62 测试及 ASan / UBSan 通过，并发时间戳场景连续重复 100 次通过，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 63：TupleMeta。

## 阶段 63：TupleMeta

- 做了什么：为每个物理 RID 增加独立持久化 TupleMeta，保存 timestamp 与 is_deleted，Record/Tuple 逻辑序列化保持不变。
- 关键设计：SlottedPage v2 将元数据编码在 Slot 中；v1 页面保持可读并提供默认元数据，空间允许时原页升级且 RID 稳定；元数据写入沿用 Page Guard 与 WAL 写入口。
- 测试结果：全新构建无警告，63 / 63 测试及 ASan / UBSan 通过，覆盖二进制 Record 隔离、重开持久化、v1 兼容升级和失败原子性，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 64：Undo Version Chain。

## 阶段 64：Undo Version Chain

- 做了什么：实现 Transaction 持有的 UndoRecord 和 TransactionManager 管理的 RID 到 VersionLink 链头映射。
- 关键设计：每个旧版本完整保存 Record、TupleMeta 与前驱链接；提交保留历史供旧 snapshot 使用；回滚移除本事务版本并恢复原链头。
- 测试结果：全新构建无警告，64 / 64 测试及 ASan / UBSan 通过，覆盖多版本链、独立 RID、二进制旧值、提交保留与回滚恢复，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 65：Version Reconstruction。

## 阶段 65：Version Reconstruction

- 做了什么：实现按 read timestamp 从当前物理 tuple 和 Undo 链重建可见 RecordVersion。
- 关键设计：选择不晚于 snapshot 的最近版本；可见 tombstone 和插入前 snapshot 返回无记录；单次重建持有链锁并检测悬空、跨 RID 与循环链接。
- 测试结果：全新构建无警告，65 / 65 测试及 ASan / UBSan 通过，覆盖当前/历史/二进制版本、删除与重新插入可见性及无历史边界，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 66：Snapshot Reads。

## 阶段 66：Snapshot Reads

- 做了什么：新增 Snapshot Isolation，并让 SeqScan、IndexScan、RangeScan、Join 与 Aggregate 按 transaction read_ts 读取可见版本。
- 关键设计：ExecutionContext 携带 TransactionManager；TableScan 统一重建版本；MVCC IndexOnlyScan 暂时安全回表校验可见性，snapshot reader 不获取行 S 锁。
- 测试结果：全新构建无警告，66 / 66 测试及 ASan / UBSan 通过，覆盖历史更新、future row、tombstone、全部扫描路径和 SQL BEGIN SNAPSHOT，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 67：MVCC DML。

## 阶段 67：MVCC DML

- 做了什么：让 INSERT / UPDATE / DELETE 创建 MVCC 当前版本与 UndoRecord，DELETE 使用逻辑 tombstone，并在提交时盖 commit timestamp。
- 关键设计：TupleMeta 高位编码未提交 transaction_id；写事务注册 RID 后由 COMMIT 在 durable 前统一盖时间戳；ABORT 复用 full-page rollback 恢复表和索引并撤销 Undo 链。
- 测试结果：全新构建无警告，67 / 67 测试及 ASan / UBSan 通过，覆盖 read-your-own-write、旧 snapshot、commit stamping、DML rollback、索引一致性与重开持久化，git diff --check 通过。
- Commit：见 Git 历史
- 下一步：阶段 68：Write Conflict。
