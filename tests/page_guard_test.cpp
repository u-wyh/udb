#include "udb/buffer_pool_manager.h"

#include <chrono>
#include <iostream>
#include <type_traits>

namespace {
using namespace udb;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template <typename Error, typename Function>
void Reject(Function function) {
    try { function(); } catch (const Error&) { return; }
    throw std::runtime_error("Expected error");
}

void TestGuards(const std::filesystem::path& path) {
    static_assert(!std::is_copy_constructible_v<ReadPageGuard>);
    static_assert(!std::is_copy_constructible_v<WritePageGuard>);
    static_assert(std::is_same_v<decltype(std::declval<ReadPageGuard&>().GetPage()), const Page&>);

    DiskManager disk(path);
    BufferPoolManager pool(disk, 1);
    page_id_t page_id;
    {
        auto guard = pool.NewPageGuard();
        page_id = guard.GetPageId();
        guard.GetPage().data[0] = 'g';
        Reject<std::runtime_error>([&] { pool.NewPageGuard(); });
        auto moved = std::move(guard);
        Check(!guard.IsValid() && moved.IsValid() && moved.GetPageId() == page_id,
              "Write guard move lost ownership");
        Reject<std::logic_error>([&] { guard.GetPage(); });
    }
    pool.FlushAllPages();
    Check(disk.ReadPage(page_id).data[0] == 'g', "Write guard did not mark the page dirty");

    {
        auto read = pool.ReadPage(page_id);
        Check(read.GetPage().data[0] == 'g', "Read guard returned wrong data");
        Reject<std::runtime_error>([&] { pool.NewPageGuard(); });
        read.Drop();
        Check(!read.IsValid(), "Explicit guard drop retained ownership");
        read.Drop();
        Reject<std::logic_error>([&] { read.GetPageId(); });
    }
    {
        try {
            auto write = pool.WritePage(page_id);
            write.GetPage().data[1] = 'x';
            throw std::runtime_error("fixture");
        } catch (const std::runtime_error&) {
        }
        auto next = pool.NewPageGuard();
        Check(next.GetPageId() != page_id, "Exception leaked a page pin");
    }
    pool.FlushAllPages();
    Check(disk.ReadPage(page_id).data[1] == 'x', "Exceptional write guard did not preserve dirtiness");
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-page-guard-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        TestGuards(directory / "guard.udb");
        std::cout << "Page guard tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
