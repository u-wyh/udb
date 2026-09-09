# UDB

C++17 关系型数据库学习项目，目前支持页存储、Buffer Pool、SlottedPage、TableHeap、基础类型与可恢复 Catalog。

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`Database::Create("test.udb")` 创建数据库，`Database::Open("test.udb")` 恢复已有数据库；二者返回拥有实例的 `unique_ptr`，通过 `GetCatalog()` 访问表。

调用方必须显式 `Flush()` 或 `Close()`：先刷数据页，再将元数据写入 `.meta.tmp`，flush/close 后 rename 替换 `.meta`。Close 成功后先前取得的引用失效；析构不自动保存。一次只允许一个实例操作同一数据库。

独立 `.meta` 是当前 bootstrap 设计，未来可能演进为统一数据库文件；目前不承诺 fsync 持久性或事务级崩溃恢复。若上次保存留下 `.meta.tmp`，新的保存会明确报错，不静默覆盖它。
