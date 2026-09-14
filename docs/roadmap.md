# UDB Roadmap v5

## ARIES Recovery

- 85. Durable PageLSN Foundation：为每个数据页建立持久化 ；保持逻辑 /SlottedPage/B+Tree 格式不被强制重写，可使用安全的 sidecar 或等价设计；补齐真正的 WAL-before-data durability / fsync 顺序。
- 86. WAL Transaction Chains：每条事务日志增加 ；Transaction 维护 ；新增 CLR（Compensation Log Record）和 ，并版本化 WAL 格式。
- 87. ARIES Analysis Pass：实现 restart Analysis，从 checkpoint/WAL 重建 Transaction Table 和 Dirty Page Table，识别 winner / loser transaction。
- 88. ARIES Redo Pass：从最小  开始 repeat history；根据 DPT 与持久化 pageLSN 判断是否真正重做 PageWrite / Allocate / Free；redo 必须幂等。
- 89. ARIES Undo Pass：按最大 LSN 优先逆序 undo loser transaction；每次 undo 写 CLR；支持多个并发 loser；最终写 ABORT/END 等价终止状态。
- 90. Crash During Recovery：支持在 Undo 中再次 crash；重启后通过 CLR / undoNextLSN 从正确位置继续，不重复撤销已完成动作。

## Checkpoint / Log Lifecycle

- 91. Fuzzy Checkpoint：实现允许活跃事务存在的 checkpoint；记录必要的 Transaction Table / DPT 状态，不再因为 active transaction 拒绝 checkpoint。
- 92. WAL Truncation：根据 checkpoint、DPT  和活跃事务安全确定最早必需日志；只回收确定不再需要的 WAL，保证 restart recovery 正确。

## Final Recovery Stress

- 93. ARIES + MVCC + SSI Stress：综合测试：
  - 多并发 committed / loser transaction
  - 同页不同 RID
  - page allocation/free
  - B+ Tree split/merge
  - MVCC update/delete
  - SSI abort
  - fuzzy checkpoint
  - crash before/after WAL flush
  - crash during redo
  - crash during undo
  - 连续多次 crash/reopen
  - Vacuum
  - WAL truncation
  - timestamp 单调
  - index/table 最终一致性
  压力场景重复运行并验证 recovery 幂等。

# STOP

阶段 93 完成后停止。

不要自行开始：

- Foreign Key / CHECK constraints
- Transactional DDL
- Distributed transaction
- Replication
- Vector Index
- Buffer Pool 性能重构

阶段 93 后重新评审 SQL 完整性、存储性能与后续高级功能路线。
