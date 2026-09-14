#include "udb/buffer_pool_manager.h"
#include "udb/transaction.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace udb;

void Check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void Put(std::vector<unsigned char>& bytes, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        bytes.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xff));
    }
}

std::uint32_t Checksum(const std::vector<unsigned char>& bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

void WriteLegacyRecord(std::ofstream& output, LogRecordType type, lsn_t lsn,
                       transaction_id_t transaction_id) {
    std::vector<unsigned char> bytes;
    Put(bytes, 0x31304c4157424455ULL, 8);
    Put(bytes, 1, 4);
    Put(bytes, 48, 4);
    Put(bytes, 0, 4);
    Put(bytes, static_cast<std::uint8_t>(type), 1);
    Put(bytes, 0, 3);
    Put(bytes, lsn, 8);
    Put(bytes, transaction_id, 8);
    Put(bytes, std::numeric_limits<std::uint64_t>::max(), 8);
    const auto checksum = Checksum(bytes);
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[16 + i] = static_cast<unsigned char>((checksum >> (8 * i)) & 0xff);
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void TestTransactionChainsAndClr(const std::filesystem::path& directory) {
    const auto wal_path = directory / "chain.wal";
    const auto data_path = directory / "chain.udb";
    {
        LogManager log(wal_path);
        DiskManager disk(data_path);
        BufferPoolManager pool(disk, 2, &log);
        TransactionManager transactions(&pool, &log);
        auto& transaction = transactions.Begin();
        pool.SetActiveTransaction(&transaction);
        {
            auto page = pool.NewPageGuard();
            page.GetPage().data[0] = 'c';
        }
        pool.SetActiveTransaction(nullptr);
        transactions.Commit(transaction);
        const auto& records = log.GetRecords();
        Check(records.size() == 4 && !records[0].GetPrevLsn() &&
                  records[1].GetPrevLsn() == 0 && records[2].GetPrevLsn() == 1 &&
                  records[3].GetPrevLsn() == 2 && transaction.GetLastLsn() == 3,
              "Transaction WAL chain or lastLSN is incorrect");

        Page compensation;
        compensation.data.fill('u');
        Check(log.Append(LogRecord::Begin(999)) == 4 &&
                  log.Append(LogRecord::Compensation(999, 0,
                      CompensationType::PageWrite, compensation, 1)) == 5 &&
                  log.Append(LogRecord::Abort(999)) == 6,
              "CLR fixture LSNs are wrong");
        log.Flush();
    }
    {
        LogManager log(wal_path);
        const auto& clr = log.GetRecords().at(5);
        Check(clr.GetType() == LogRecordType::Compensation && clr.GetPrevLsn() == 4 &&
                  clr.GetUndoNextLsn() == 1 &&
                  clr.GetCompensationType() == CompensationType::PageWrite &&
                  clr.GetAfterImage() && clr.GetAfterImage()->data[0] == 'u' &&
                  log.GetRecords().at(6).GetPrevLsn() == 5,
              "Versioned CLR did not survive WAL reopen");
    }
}

void TestLegacyWalCompatibility(const std::filesystem::path& path) {
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        WriteLegacyRecord(output, LogRecordType::Begin, 0, 77);
        WriteLegacyRecord(output, LogRecordType::Commit, 1, 77);
    }
    {
        LogManager log(path);
        Check(log.GetRecords().size() == 2 && !log.GetRecords()[0].GetPrevLsn() &&
                  log.GetRecords()[1].GetPrevLsn() == 0,
              "Legacy WAL transaction chain was not synthesized");
        Check(log.Append(LogRecord::Begin(78)) == 2,
              "Versioned WAL could not append after legacy records");
        log.Append(LogRecord::Abort(78));
        log.Flush();
    }
    LogManager reopened(path);
    Check(reopened.GetRecords().size() == 4 && reopened.GetRecords()[3].GetPrevLsn() == 2,
          "Mixed legacy/current WAL did not reopen");
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("udb-wal-chain-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(directory), "Cannot create test directory");
        struct Cleanup { std::filesystem::path path; ~Cleanup() {
            std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
        TestTransactionChainsAndClr(directory);
        TestLegacyWalCompatibility(directory / "legacy.wal");
        std::cout << "WAL transaction chain tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
