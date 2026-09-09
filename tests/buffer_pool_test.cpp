#include "udb/buffer_pool_manager.h"

#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function>
void ExpectThrow(Function function) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error("Expected exception was not thrown");
}

struct TemporaryDirectory {
    std::filesystem::path path;

    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            path = std::filesystem::temp_directory_path() /
                   ("udb-buffer-test-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(path)) {
                return;
            }
        }
        throw std::runtime_error("Cannot create temporary test directory");
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

udb::Page Pattern(char value) {
    udb::Page page;
    page.data.fill(value);
    return page;
}

void TestPinsAndErrors(const std::filesystem::path& path) {
    udb::DiskManager disk(path);
    ExpectThrow<std::invalid_argument>([&] { udb::BufferPoolManager pool(disk, 0); });
    udb::BufferPoolManager pool(disk, 1);
    ExpectThrow<std::out_of_range>([&] { pool.FetchPage(0); });
    const auto [id, page] = pool.NewPage();
    Check(id == 0 && page->data == udb::Page{}.data, "NewPage must allocate a zero page");
    Check(pool.FetchPage(id) == page, "Fetch must reuse the resident page");
    *page = Pattern('a');
    pool.UnpinPage(id, true);
    // The second pin still protects the only frame.
    ExpectThrow<std::runtime_error>([&] { pool.NewPage(); });
    ExpectThrow<std::runtime_error>([&] { pool.FetchPage(1); });
    Check(std::filesystem::file_size(path) == udb::PAGE_SIZE, "Failed NewPage grew disk");
    pool.UnpinPage(id, false);
    ExpectThrow<std::logic_error>([&] { pool.UnpinPage(id, true); });
    ExpectThrow<std::out_of_range>([&] { pool.UnpinPage(42, false); });
    ExpectThrow<std::out_of_range>([&] { pool.FlushPage(42); });
    for (const auto invalid : {udb::page_id_t{-1}, udb::page_id_t{1},
                               std::numeric_limits<udb::page_id_t>::max()}) {
        ExpectThrow<std::out_of_range>([&] { pool.FetchPage(invalid); });
    }
    Check(pool.FetchPage(id) == page && page->data == Pattern('a').data,
          "Failed fetch changed the resident page");
    pool.UnpinPage(id, false);
    const auto [next_id, next] = pool.NewPage();
    Check(next_id == 1 && next == page, "Capacity one must reuse its only frame");
    Check(disk.ReadPage(id).data == Pattern('a').data, "Dirty eviction lost data");
    ExpectThrow<std::out_of_range>([&] { pool.UnpinPage(id, false); });
    pool.UnpinPage(next_id, false);
    Check(pool.FetchPage(id)->data == Pattern('a').data, "Evicted page cannot be reloaded");
    pool.UnpinPage(id, false);
}

void TestLru(const std::filesystem::path& path) {
    udb::DiskManager disk(path);
    udb::BufferPoolManager pool(disk, 3);
    const auto [a, pa] = pool.NewPage();
    const auto [b, pb] = pool.NewPage();
    const auto [c, pc] = pool.NewPage();
    Check(pa != pb && pb != pc && pa != pc, "New pages must have distinct frames");
    pool.UnpinPage(a, false);
    pool.UnpinPage(b, false);
    pool.UnpinPage(c, false);
    Check(pool.FetchPage(a) == pa, "Resident hit must reuse frame");
    pool.UnpinPage(a, false);  // LRU order: b, c, a.
    const auto [d, pd] = pool.NewPage();
    Check(pd == pb, "LRU must evict b after a is accessed again");
    ExpectThrow<std::out_of_range>([&] { pool.UnpinPage(b, false); });
    // Leave d pinned. Access c (pinned), then a (unpinned): d, c, a.
    Check(pool.FetchPage(c) == pc, "c should still be resident");
    Check(pool.FetchPage(a) == pa, "a should still be resident");
    pool.UnpinPage(a, false);
    const auto [e, pe] = pool.NewPage();
    Check(pe == pa, "Pinned older pages must be skipped by LRU");
    ExpectThrow<std::runtime_error>([&] { pool.NewPage(); });
    pool.UnpinPage(d, false);
    pool.UnpinPage(c, false);
    pool.UnpinPage(e, false);
    // Unpin order must not change recency: d is still least recently accessed.
    const auto [f, pf] = pool.NewPage();
    Check(pf == pd, "Unpin must not change last-access LRU order");
    pool.UnpinPage(f, false);
}

void TestFlushAndReopen(const std::filesystem::path& path) {
    {
        udb::DiskManager disk(path);
        udb::BufferPoolManager pool(disk, 2);
        const auto [a, pa] = pool.NewPage();
        *pa = Pattern('a');
        pool.FlushPage(a);  // Explicitly flush a currently pinned page.
        Check(disk.ReadPage(a).data == Pattern('a').data, "FlushPage failed");
        *pa = Pattern('b');
        pool.UnpinPage(a, true);  // A later change must make it dirty again.
        const auto [b, pb] = pool.NewPage();
        *pb = Pattern('c');
        pool.UnpinPage(b, true);
        pool.FlushAllPages();
        Check(disk.ReadPage(a).data == Pattern('b').data, "FlushAllPages missed a");
        Check(disk.ReadPage(b).data == Pattern('c').data, "FlushAllPages missed b");
        const auto [c, pc] = pool.NewPage();
        *pc = Pattern('d');
        pool.UnpinPage(c, true);
        const auto [d, pd] = pool.NewPage();
        Check(pd->data == udb::Page{}.data, "Reused frame must be zeroed for new page");
        pool.UnpinPage(d, false);
        const auto [e, pe] = pool.NewPage();  // Evict dirty c.
        Check(pe != nullptr, "NewPage returned null");
        pool.UnpinPage(e, false);
    }
    {
        udb::DiskManager disk(path);
        udb::BufferPoolManager pool(disk, 1);
        for (const auto id : {udb::page_id_t{0}, udb::page_id_t{1}, udb::page_id_t{2}}) {
            Check(pool.FetchPage(id)->data == Pattern(static_cast<char>('b' + id)).data,
                  "Flush/eviction data did not survive reopen");
            pool.UnpinPage(id, false);
        }
    }
}

void TestCleanIo(const std::filesystem::path& path) {
    udb::DiskManager disk(path);
    const auto a = disk.AllocatePage();
    const auto b = disk.AllocatePage();
    udb::BufferPoolManager pool(disk, 1);
    auto* page = pool.FetchPage(a);
    pool.UnpinPage(a, false);
    // Deliberate test-only disk mutation makes redundant reads/writes observable.
    // Production callers must not bypass a pool's cached pages in this way.
    disk.WritePage(a, Pattern('x'));
    Check(pool.FetchPage(a) == page && page->data == udb::Page{}.data,
          "Cache hit unexpectedly read disk");
    pool.UnpinPage(a, false);
    pool.FlushAllPages();
    Check(disk.ReadPage(a).data == Pattern('x').data, "Clean page was flushed");
    pool.FetchPage(b);
    pool.UnpinPage(b, false);
    Check(disk.ReadPage(a).data == Pattern('x').data, "Clean victim was written");
    page = pool.FetchPage(a);
    *page = Pattern('y');
    pool.UnpinPage(a, true);
    pool.FlushAllPages();
    disk.WritePage(a, Pattern('z'));
    pool.FlushAllPages();
    Check(disk.ReadPage(a).data == Pattern('z').data, "FlushAllPages did not clear dirty");
    page = pool.FetchPage(a);
    *page = Pattern('v');
    pool.UnpinPage(a, true);
    pool.FlushPage(a);
    Check(disk.ReadPage(a).data == Pattern('v').data, "Explicit dirty flush failed");
    disk.WritePage(a, Pattern('w'));
    pool.FetchPage(b);
    Check(disk.ReadPage(a).data == Pattern('w').data, "FlushPage did not clear dirty");
    pool.UnpinPage(b, false);
}

void TestReadFailure(const std::filesystem::path& path) {
    udb::DiskManager disk(path);
    disk.AllocatePage();
    disk.AllocatePage();
    udb::BufferPoolManager pool(disk, 1);
    auto* original = pool.FetchPage(0);
    *original = Pattern('r');
    pool.UnpinPage(0, true);
    std::filesystem::resize_file(path, udb::PAGE_SIZE + 1);
    ExpectThrow<std::runtime_error>([&] { pool.FetchPage(1); });
    Check(pool.FetchPage(0) == original && original->data == Pattern('r').data,
          "Failed disk read damaged victim or mapping");
    pool.UnpinPage(0, false);
    pool.FlushAllPages();
    Check(disk.ReadPage(0).data == Pattern('r').data, "Failed read lost victim dirty state");
}

}  // namespace

int main() {
    try {
        TemporaryDirectory temporary;
        TestPinsAndErrors(temporary.path / "pins.udb");
        TestLru(temporary.path / "lru.udb");
        TestFlushAndReopen(temporary.path / "flush.udb");
        TestCleanIo(temporary.path / "clean.udb");
        TestReadFailure(temporary.path / "failure.udb");
        std::cout << "Buffer pool tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
