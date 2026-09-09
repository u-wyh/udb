#include "udb/disk_manager.h"

#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void Check(bool condition) {
    if (!condition) {
        throw std::runtime_error("Test check failed");
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
                   ("udb-test-" + std::to_string(stamp) + "-" + std::to_string(attempt));
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

void TestStorage() {
    TemporaryDirectory temporary;
    const auto path = temporary.path / "database.udb";
    udb::Page first;
    udb::Page second;
    for (std::size_t i = 0; i < udb::PAGE_SIZE; ++i) {
        first.data[i] = static_cast<char>(i % 127);
        second.data[i] = static_cast<char>((i + 13) % 127);
    }
    const udb::Page zero;
    {
        udb::DiskManager disk(path);
        Check(std::filesystem::exists(path));
        Check(std::filesystem::file_size(path) == 0);
        ExpectThrow<std::out_of_range>([&] { disk.ReadPage(0); });
        for (udb::page_id_t id = 0; id < 3; ++id) {
            Check(disk.AllocatePage() == id);
            Check(disk.ReadPage(id).data == zero.data);
        }
        Check(std::filesystem::file_size(path) == 3 * udb::PAGE_SIZE);
        disk.WritePage(0, first);
        disk.WritePage(1, second);
        Check(disk.ReadPage(0).data == first.data);
        Check(disk.ReadPage(1).data == second.data);
        disk.WritePage(0, second);
        Check(disk.ReadPage(0).data == second.data);
        disk.WritePage(0, first);
        for (const auto id : {udb::page_id_t{-1}, udb::page_id_t{3},
                              std::numeric_limits<udb::page_id_t>::max()}) {
            ExpectThrow<std::out_of_range>([&] { disk.ReadPage(id); });
            ExpectThrow<std::out_of_range>([&] { disk.WritePage(id, first); });
        }
        Check(std::filesystem::file_size(path) == 3 * udb::PAGE_SIZE);
        Check(disk.ReadPage(0).data == first.data);
    }
    {
        udb::DiskManager disk(path);
        Check(disk.ReadPage(0).data == first.data);
        Check(disk.ReadPage(1).data == second.data);
        Check(disk.ReadPage(2).data == zero.data);
        Check(disk.AllocatePage() == 3);
        Check(disk.ReadPage(3).data == zero.data);
    }
    // Verify the physical offset independently of DiskManager's read path.
    {
        std::ifstream raw(path, std::ios::binary);
        raw.seekg(static_cast<std::streamoff>(udb::PAGE_SIZE));
        udb::Page actual;
        raw.read(actual.data.data(), static_cast<std::streamsize>(udb::PAGE_SIZE));
        Check(static_cast<bool>(raw));
        Check(actual.data == second.data);
    }
    ExpectThrow<std::runtime_error>([&] {
        udb::DiskManager disk(temporary.path / "missing" / "database.udb");
    });
    ExpectThrow<std::runtime_error>([&] { udb::DiskManager disk(temporary.path); });
    const auto malformed = temporary.path / "malformed.udb";
    {
        std::ofstream file(malformed, std::ios::binary);
        file.put('x');
    }
    ExpectThrow<std::runtime_error>([&] { udb::DiskManager disk(malformed); });
    Check(std::filesystem::file_size(malformed) == 1);
    // An unexpectedly truncated file must produce an I/O error, not a partial page.
    {
        udb::DiskManager disk(path);
        std::filesystem::resize_file(path, udb::PAGE_SIZE + 1);
        ExpectThrow<std::runtime_error>([&] { disk.ReadPage(1); });
    }
}

}  // namespace

int main() {
    try {
        TestStorage();
        std::cout << "Storage tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
