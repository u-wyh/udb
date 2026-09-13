# UDB Roadmap v3

## MVCC 前正确性地基

- 61. Same-page Transaction Safety：补齐两个事务修改同一物理页不同 RID 时的 abort/crash 测试；保证现有 full-page WAL / rollback 不会互相覆盖。必要时增加 transaction-lifetime page write ownership。
- 62. MVCC Timestamp Core：Transaction 增加 ，实现全局 commit timestamp 和 active transaction watermark。

## Version Storage

- 63. TupleMeta：为 RID 对应的物理 tuple 增加独立 ；不得污染 Tuple/Record 逻辑格式；持久化格式版本化并处理现有数据库兼容。
- 64. Undo Version Chain：实现 transaction-owned UndoRecord 和 RID → VersionLink；旧版本保存完整旧 Record + TupleMeta。
- 65. Version Reconstruction：给定 transaction snapshot，从当前 tuple 沿 undo chain 重建其可见版本。

## Snapshot Isolation

- 66. Snapshot Reads：新增 Snapshot Isolation；SELECT/TableScan/IndexScan 根据  做 MVCC visibility；snapshot reader 不获取普通行 S 锁。
- 67. MVCC DML：INSERT / UPDATE / DELETE 创建版本；DELETE 使用 logical tombstone；支持 read-your-own-write、commit stamping、abort undo。
- 68. Write Conflict：实现 first-writer/first-committer 冲突检测，阻止 lost update，并保证 unique constraint 在并发版本下正确。

## Index + MVCC

- 69. Version-aware Index：indexed column 更新/删除时保留旧 snapshot 必需的 index entry；查询后做 version visibility/residual validation；兼容 non-unique/composite/VARCHAR index。
- 70. MVCC Index-only Scan：只有能够证明目标版本可见且 index entry 足够新时使用 covering result，否则安全回退 tuple lookup。

## GC / Persistence

- 71. Vacuum + Watermark：依据 oldest active snapshot 清理不可再见 UndoRecord、logical tombstone 和 stale index entries；物理删除后 RID 仍不得错误复用。
- 72. MVCC WAL / Recovery：验证 tuple meta、current version、commit timestamp 与 logical delete 在 crash recovery 后正确；恢复后无需保留已死亡 snapshot 的历史链。
- 73. MVCC Checkpoint / Reopen：checkpoint、vacuum、WAL 回收及 reopen 后 timestamp 单调性与索引一致性。

## Isolation Integration

- 74. MVCC READ COMMITTED：每条 statement 获取新 snapshot，实现 RC 的 non-repeatable read 语义，不依赖读取 S 锁。
- 75. MVCC REPEATABLE READ：整个 transaction 固定 snapshot；保留现有写冲突保护，验证 repeatable read 与 phantom 行为。
- 76. Snapshot Isolation Semantics：完整验证 dirty read、lost update、read-your-writes、delete/update visibility，并明确测试 SI 允许的 write-skew。

## Final Stress

- 77. MVCC Concurrent Stress：多 reader/writer、多个 index、长事务、vacuum、abort、deadlock、checkpoint、crash/reopen 综合压力测试，并连续重复运行。

# STOP

阶段 77 完成后停止。

不要自行实现：

- Serializable Snapshot Isolation
- Predicate Lock
- SSI conflict graph
- 分布式事务
- Replication
- Vector Index

完成阶段 77 后重新评审 MVCC、WAL、Optimizer 和存储格式，再制定下一版 roadmap。
