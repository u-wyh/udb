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
