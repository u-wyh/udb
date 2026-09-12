# UDB Roadmap v2

原则：正确性优先；每阶段独立可测试、可回滚；一个阶段一个 commit。

## 事务基础

- 47. Page Guard：实现统一 Read/Write Page Guard，替换存储层零散 pin/unpin RAII，为事务、WAL 和 latch 提供统一写入口。
- 48. Transaction Core：实现 transaction_id、Transaction 状态、TransactionManager 和 ExecutionContext；SQL 执行链携带当前 Transaction。
- 49. Explicit Transactions：支持 BEGIN / COMMIT / ROLLBACK 和 autocommit；通过 page before-image + allocate/free write set 实现单线程事务回滚；显式事务内暂时拒绝 DDL。

## WAL 与恢复

- 50. WAL Foundation：新增 `.wal`、LogManager、LSN、BEGIN / PAGE_WRITE / PAGE_ALLOC / PAGE_FREE / COMMIT / ABORT 日志格式及校验。
- 51. Write-Ahead Rule：WritePageGuard 接入 physical full-page before/after WAL；脏页刷盘前必须先持久化对应 WAL；COMMIT 日志必须 durable 后才能确认提交。
- 52. Crash Recovery：启动时扫描 WAL，REDO committed transaction，反向 UNDO loser transaction，同时恢复 page allocate/free；支持 truncated WAL 和重复 recovery。
- 53. Checkpoint：实现 checkpoint、数据/meta/WAL 的安全刷盘顺序及 WAL 回收，缩短恢复扫描范围。

## 并发基础

- 54. Thread-safe Storage：DiskManager、BufferPoolManager、Page Guard 增加必要 mutex/latch；支持并发 pin/fetch/flush。
- 55. Concurrent Table/Index：让 TableHeap、B+ Tree 和 Catalog 在现有模块边界下安全支持并发访问。
- 56. Lock Manager：实现 table/row Shared / Exclusive lock、兼容矩阵、lock upgrade 与事务持锁集合。
- 57. Strict 2PL：SELECT/DML 接入 LockManager；写锁持有到事务结束，COMMIT/ROLLBACK 统一释放；默认 Repeatable Read。
- 58. Deadlock：实现 wait-for graph、cycle detection、victim abort，并通过已有 rollback/WAL 路径恢复。
- 59. Isolation Levels：支持 READ COMMITTED 与 REPEATABLE READ，并验证 dirty read、non-repeatable read 等隔离语义。
- 60. Concurrent Recovery Stress：多 session 并发事务 + TableHeap + B+ Tree + WAL + crash/reopen 综合压力测试，修复最终一致性问题。

# STOP

完成阶段 60 后停止。

不要自行开始 MVCC / Snapshot Isolation / Version Chain / Vacuum。

阶段 60 后需要重新评审 tuple layout、RID 稳定性、index entry 与 transaction timestamp 设计，再制定 MVCC roadmap。
