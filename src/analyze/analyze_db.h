#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace svg_mb_control::analyze {

constexpr int kSchemaVersion = 13;

class SqliteError : public std::runtime_error {
  public:
    SqliteError(std::string message, int code);
    int code() const noexcept { return code_; }

  private:
    int code_;
};

class Statement {
  public:
    Statement() = default;
    Statement(sqlite3* db, std::string_view sql);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    void BindNull(int idx);
    void BindInt(int idx, std::int64_t value);
    void BindDouble(int idx, double value);
    void BindText(int idx, std::string_view value);
    void BindOptionalInt(int idx, std::optional<std::int64_t> value);
    void BindOptionalDouble(int idx, std::optional<double> value);
    void BindOptionalText(int idx, const std::optional<std::string>& value);

    bool Step();
    void Reset();

    std::int64_t ColumnInt(int idx) const;
    double ColumnDouble(int idx) const;
    std::string ColumnText(int idx) const;
    bool ColumnIsNull(int idx) const;

    sqlite3_stmt* handle() const { return stmt_; }

  private:
    void Finalize();

    sqlite3_stmt* stmt_ = nullptr;
};

class Transaction {
  public:
    explicit Transaction(sqlite3* db);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void Commit();

  private:
    sqlite3* db_;
    bool committed_ = false;
};

class Database {
  public:
    Database() = default;
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void Open(const std::filesystem::path& path);
    void Close();

    void Exec(std::string_view sql);
    Statement Prepare(std::string_view sql);
    std::int64_t LastInsertRowId() const;

    sqlite3* handle() const { return db_; }

  private:
    sqlite3* db_ = nullptr;
};

void BootstrapSchema(Database& db);
int GetSchemaVersion(Database& db);

}  // namespace svg_mb_control::analyze
