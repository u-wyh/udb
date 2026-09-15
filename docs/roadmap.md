# UDB Roadmap v6

## Persistent System Catalog

- 94. System Catalog Storage：把 Table / Column / Index 等 schema metadata 建立为正式的 on-disk catalog storage；仅保留最小 bootstrap metadata，不再把完整 Catalog 状态依赖独立 `.meta` 快照。
- 95. Catalog Migration：兼容并迁移现有 metadata v1–v5 数据库；验证 reopen、free-page state、index root、commit timestamp 与现有 ARIES checkpoint 不回退。
- 96. Transactional Catalog：Catalog mutation 接入 Transaction + WAL/ARIES；CREATE/DROP 的 catalog 修改可以 commit、rollback，并能在 crash recovery 后恢复正确状态。
- 97. Transactional DDL：允许显式事务中的 CREATE TABLE / DROP TABLE / CREATE INDEX / DROP INDEX；增加必要 schema/catalog locking，保证并发 SQL 不读取半完成 schema。

## Constraint Framework

- 98. Column Constraints：实现持久化 `NOT NULL` 与 `DEFAULT` metadata；INSERT/UPDATE 和 schema reopen 后语义一致。
- 99. PRIMARY KEY / UNIQUE：SQL Parser/Binder/Catalog 支持 PRIMARY KEY 与 UNIQUE constraint；自动建立或复用 unique index；PRIMARY KEY 隐含 NOT NULL；约束参与事务、MVCC、rollback 和 recovery。
- 100. CHECK Constraint：支持列/表级 CHECK；保存可重建的约束表达式；INSERT/UPDATE 使用 SQL 三值逻辑验证，FALSE 拒绝、TRUE/NULL 按 SQL CHECK 语义处理。
- 101. FOREIGN KEY 基础：支持单列及可自然扩展的 metadata；INSERT/UPDATE 验证 referenced key；DELETE/UPDATE referenced row 先实现 RESTRICT / NO ACTION；所有检查使用当前事务可见版本。
- 102. Foreign Key Actions：实现 `ON DELETE / ON UPDATE CASCADE` 与 `SET NULL` 的基础行为；处理级联链、循环检测、事务 rollback 和 unique/index consistency。

## Final DDL / Constraint Stress

- 103. Catalog + Constraint Stress：综合验证：
  - 显式事务 DDL commit/rollback
  - DDL 与并发查询
  - crash during CREATE/DROP
  - fuzzy checkpoint
  - ARIES redo/undo
  - PRIMARY KEY / UNIQUE
  - NOT NULL / DEFAULT
  - CHECK
  - FOREIGN KEY
  - CASCADE / SET NULL
  - MVCC snapshots
  - Serializable transactions
  - index consistency
  - reopen / repeated crash recovery
  - legacy database migration
  压力场景重复运行并验证 Catalog、数据、索引、约束始终一致。

# STOP

阶段 103 完成后停止。

不要自行开始：

- Buffer Pool replacement 性能重构
- B+ Tree latch crabbing
- Query optimizer 大规模重写
- Distributed transaction
- Replication
- Vector Index

阶段 103 后重新评审 storage performance 与 optimizer，再制定下一版 roadmap。
