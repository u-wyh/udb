# UDB Roadmap v4

## Serializable Snapshot Isolation

- 78. SSI Transaction Core：新增 SERIALIZABLE isolation level；为事务加入 SSI read/write dependency 状态，并允许已提交事务的必要冲突信息暂时存活。
- 79. Tuple SIREAD：记录 Serializable transaction 对 RID 的非阻塞 SIREAD；writer 修改被其他并发事务读取的 RID 时建立 rw-antidependency。
- 80. Predicate / Range SIREAD：Index point/range scan 记录 key/range SIREAD；SeqScan 或无法精确描述的 predicate 可保守记录 table-level SIREAD；INSERT/UPDATE/DELETE 检测 phantom rw-conflict。
- 81. SSI Dangerous Structure Detection：维护必要的 rw dependency，识别可能形成 serialization anomaly 的 dangerous structure；在 commit/冲突阶段选择事务 abort。允许保守 abort，不允许 write skew 错误提交。
- 82. Serializable SQL Integration：支持 ；SeqScan、IndexScan、RangeScan、Join、Aggregate 等执行路径统一注册 SSI reads；DML 注册 writes。
- 83. SSI GC：利用 transaction 生命周期、commit timestamp 和 watermark 清理不再可能参与冲突的 SIREAD / dependency metadata，避免长期增长。
- 84. Serializable Stress：综合验证：
  - write skew 必须至少 abort 一个事务
  - phantom anomaly 被阻止
  - point/range/table SIREAD
  - reader 不阻塞 writer
  - writer/write conflict
  - long snapshot
  - index / composite index
  - abort/deadlock
  - Vacuum
  - checkpoint
  - crash/reopen
  - 多线程重复压力测试

# STOP

完成阶段 84 后停止。

不要自行开始：

- Transactional DDL
- Foreign Key / CHECK / UNIQUE constraint framework
- ARIES WAL 重构
- Distributed transaction
- Replication
- Vector Index

阶段 84 后重新评审 Serializable、WAL 和 SQL 完整性，再制定下一版 roadmap。
