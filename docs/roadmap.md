# UDB Roadmap

原则：每阶段小而完整；必须可编译、可测试、可回滚；一个阶段一个 commit。

## 当前 SQL 能力完善

- 25. ORDER BY：单列 ASC/DESC，稳定排序，NULL 规则明确，排序后再 LIMIT。（已完成）
- 26. ORDER BY 扩展：多列排序，逐列 ASC/DESC。
- 27. OFFSET：支持 LIMIT + OFFSET，三种扫描路径语义一致。
- 28. Aggregate 基础：COUNT / SUM / MIN / MAX / AVG，无 GROUP BY。
- 29. GROUP BY：单列分组 + Aggregate。
- 30. HAVING：对分组结果过滤。
- 31. SELECT 表达式与别名：支持投影 literal / column / 基础表达式及 AS。

## 多表查询

- 32. CROSS JOIN：建立最基础的双表执行能力。
- 33. INNER JOIN：支持 `JOIN ... ON` 等值条件，先使用 Nested Loop Join。
- 34. Hash Join：为等值 INNER JOIN 增加 Hash Join。
- 35. Join Planner：在 Nested Loop / Hash Join 之间进行简单规则选择。

## 执行引擎整理

- 36. Executor Operator 抽象：把扫描、过滤、投影等逐步整理为明确执行算子，保持现有 SQL 行为。
- 37. Iterator Execution：引入 `Init/Next` 或等价 Volcano-style iterator，避免查询必须一次性物化全部中间结果。
- 38. Sort / Aggregate / Join 接入统一 Executor pipeline。

## 存储与索引完善

- 39. Non-unique B+ Tree Index：一个 key 支持多个 RID，并保持现有 unique index 行为。
- 40. VARCHAR Index：设计并实现可比较的定长/变长索引 key。
- 41. Composite Index：支持多列 key 的基础表示、Catalog metadata 和查询匹配。
- 42. Index-only Scan 基础：满足条件时直接由索引提供结果。

## 查询优化基础

- 43. Statistics：表行数、基础列统计信息。
- 44. Cost Model 基础：SeqScan / IndexScan 的粗粒度成本估计。
- 45. Access Path Optimizer：基于统计选择 SeqScan / IndexScan / RangeScan。
- 46. Join Order 基础：小规模多表 join 的简单成本选择。

# STOP

完成阶段 46 后停止自动开发。

不要自行开始 Transaction / Lock Manager / WAL / Recovery / MVCC。

这些模块需要先重新评审当前存储、Executor 和 Catalog 架构，再制定下一版 roadmap。
