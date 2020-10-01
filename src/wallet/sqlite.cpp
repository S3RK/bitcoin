// Copyright (c) 2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/sqlite.h>

#include <logging.h>
#include <util/memory.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/translation.h>
#include <wallet/db.h>

#include <sqlite3.h>
#include <stdint.h>

static const char* DATABASE_FILENAME = "wallet.dat";

static Mutex g_sqlite_mutex;
static int g_sqlite_count GUARDED_BY(g_sqlite_mutex) = 0;

static void ErrorLogCallback(void* arg, int code, const char* msg)
{
    assert(arg == nullptr); // That's what we tell it to do during the setup
    LogPrintf("SQLite Error. Code: %d. Message: %s\n", code, msg);
}

SQLiteDatabase::SQLiteDatabase(const fs::path& dir_path, const fs::path& file_path, bool mock) :
    WalletDatabase(), m_mock(mock), m_dir_path(dir_path.string()), m_file_path(file_path.string())
{
    LOCK(g_sqlite_mutex);
    LogPrintf("Using SQLite Version %s\n", SQLiteDatabaseVersion());
    LogPrintf("Using wallet %s\n", m_dir_path);

    if (++g_sqlite_count == 1) {
        // Setup logging
        int ret = sqlite3_config(SQLITE_CONFIG_LOG, ErrorLogCallback, nullptr);
        if (ret != SQLITE_OK) {
            throw std::runtime_error(strprintf("SQLiteDatabase: Failed to setup error log: %s\n", sqlite3_errstr(ret)));
        }
    }
    int ret = sqlite3_initialize(); // This is a no-op if sqlite3 is already initialized
    if (ret != SQLITE_OK) {
        throw std::runtime_error(strprintf("SQLiteDatabase: Failed to initialize SQLite: %s\n", sqlite3_errstr(ret)));
    }
}

bool SQLiteDatabase::PrepareDirectory() const
{
    if (m_mock) return true;
    // Try to create the directory containing the wallet file and lock it
    TryCreateDirectories(m_dir_path);
    if (!LockDirectory(m_dir_path, ".walletlock")) {
        LogPrintf("Cannot obtain a lock on wallet directory %s. Another instance of bitcoin may be using it.\n", m_dir_path);
        return false;
    }
    return true;
}

void SQLiteDatabase::SetupSQLStatements()
{
    std::string read_sql = "SELECT value FROM main WHERE key = ?";
    std::string insert_sql = "INSERT INTO main VALUES(?, ?)";
    std::string overwrite_sql = "INSERT OR REPLACE INTO main VALUES(?, ?)";
    std::string delete_sql = "DELETE FROM main WHERE key = ?";
    std::string cursor_sql = "SELECT key, value FROM main";

    int res;
    if (!m_read_stmt) {
        if ((res = sqlite3_prepare_v2(m_db, read_sql.c_str(), -1, &m_read_stmt, nullptr)) != SQLITE_OK) {
            throw std::runtime_error(strprintf("SQLiteDatabase: Failed to setup SQL statements: %s\n", sqlite3_errstr(res)));
        }
    }
    if (!m_insert_stmt) {
        if ((res = sqlite3_prepare_v2(m_db, insert_sql.c_str(), -1, &m_insert_stmt, nullptr)) != SQLITE_OK) {
            throw std::runtime_error(strprintf("SQLiteDatabase: Failed to setup SQL statements: %s\n", sqlite3_errstr(res)));
        }
    }
    if (!m_overwrite_stmt) {
        if ((res = sqlite3_prepare_v2(m_db, overwrite_sql.c_str(), -1, &m_overwrite_stmt, nullptr)) != SQLITE_OK) {
            throw std::runtime_error(strprintf("SQLiteDatabase: Failed to setup SQL statements: %s\n", sqlite3_errstr(res)));
        }
    }
    if (!m_delete_stmt) {
        if ((res = sqlite3_prepare_v2(m_db, delete_sql.c_str(), -1, &m_delete_stmt, nullptr)) != SQLITE_OK) {
            throw std::runtime_error(strprintf("SQLiteDatabase: Failed to setup SQL statements: %s\n", sqlite3_errstr(res)));
        }
    }
    if (!m_cursor_stmt) {
        if ((res = sqlite3_prepare_v2(m_db, cursor_sql.c_str(), -1, &m_cursor_stmt, nullptr)) != SQLITE_OK) {
            throw std::runtime_error(strprintf("SQLiteDatabase: Failed to setup SQL statements : %s\n", sqlite3_errstr(res)));
        }
    }
}

SQLiteDatabase::~SQLiteDatabase()
{
    Close();

    LOCK(g_sqlite_mutex);
    if (--g_sqlite_count == 0) {
        int ret = sqlite3_shutdown();
        if (ret != SQLITE_OK) {
            LogPrintf("SQLiteDatabase: Failed to shutdown SQLite: %s\n", sqlite3_errstr(ret));
        }
    }
}

bool SQLiteDatabase::Verify(bilingual_str& error)
{
    if (!PrepareDirectory()) return false;

    sqlite3* db = nullptr;
    int ret = sqlite3_open_v2(m_file_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    if (ret == SQLITE_NOTFOUND) {
        sqlite3_close(db);
        return true; // Return true if the file doesn't exist
    } else if (ret != SQLITE_OK) {
        sqlite3_close(db);
        error = strprintf(_("SQLiteDatabase: Failed to verify database: %s"), sqlite3_errstr(ret));
        return false;
    }

    sqlite3_stmt* stmt;
    ret = sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &stmt, nullptr);
    if (ret != SQLITE_OK) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        error = strprintf(_("SQLiteDatabase: Failed to verify database: %s"), sqlite3_errstr(ret));
        return false;
    }
    while (true) {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_DONE) {
            break;
        } else if (ret != SQLITE_ROW) {
            error = strprintf(_("SQLiteDatabase: Failed to verify database: %s"), sqlite3_errstr(ret));
            break;
        }
        const char* msg = (const char*)sqlite3_column_text(stmt, 0);
        if (!msg) {
            error = strprintf(_("SQLiteDatabase: Failed to verify database: %s"), sqlite3_errstr(ret));
            break;
        }
        std::string str_msg(msg);
        if (str_msg == "ok") {
            continue;
        }
        error += Untranslated("\n" + str_msg);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return error.original.empty();
}

void SQLiteDatabase::Open(const char* mode)
{
    if (!PrepareDirectory()) {
        throw std::runtime_error("Cannot obtain a lock on wallet directory");
    }

    const bool read_only = (!strchr(mode, '+') && !strchr(mode, 'w'));

    const bool create = strchr(mode, 'c') != nullptr;
    int flags;
    if (read_only) {
        flags = SQLITE_OPEN_READONLY;
    } else {
        flags = SQLITE_OPEN_READWRITE;
    }
    if (create) {
        flags |= SQLITE_OPEN_CREATE;
    }
    if (m_mock) {
        flags |= SQLITE_OPEN_MEMORY; // In memory database for mock db
    }

    if (m_db == nullptr) {
        sqlite3* db = nullptr;
        int ret = sqlite3_open_v2(m_file_path.c_str(), &db, flags, nullptr);
        if (ret != SQLITE_OK) {
            throw std::runtime_error(strprintf("SQLiteDatabase: Failed to open database: %s\n", sqlite3_errstr(ret)));
        }
        // TODO: Maybe(?) Check the file wasn't copied and a duplicate opened

        if (create) {
            // Make the table for our key-value pairs
            std::string create_stmt = "CREATE TABLE IF NOT EXISTS main(key BLOB PRIMARY KEY, value BLOB)";
            ret = sqlite3_exec(db, create_stmt.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                throw std::runtime_error(strprintf("SQLiteDatabase: Failed to create new database: %s\n", sqlite3_errstr(ret)));
            }

            // Enable fullfysnc for the platforms that use it
            std::string fullfsync_stmt = "PRAGMA fullfsync = true";
            ret = sqlite3_exec(db, fullfsync_stmt.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                throw std::runtime_error(strprintf("SQLiteDatabase: Failed to enable fullfsync: %s\n", sqlite3_errstr(ret)));
            }
        }

        m_db = db;
    }
    if (!read_only && sqlite3_db_readonly(m_db, "main") != 0) {
        throw std::runtime_error(strprintf("SQLiteDatabase: SQLiteBatch requested read-write permission but database only has readonly"));
    }
    SetupSQLStatements();
}

bool SQLiteDatabase::Rewrite(const char* skip)
{
    while (true) {
        if (m_refcount == 0) {
            break;
        }
        UninterruptibleSleep(std::chrono::milliseconds{100});
    }

    // Rewrite the database using the VACUUM command: https://sqlite.org/lang_vacuum.html
    int ret = sqlite3_exec(m_db, "VACUUM", nullptr, nullptr, nullptr);
    return ret == SQLITE_OK;
}

bool SQLiteDatabase::PeriodicFlush()
{
    return true;
}

bool SQLiteDatabase::Backup(const std::string& dest) const
{
    sqlite3* db_copy;
    int res = sqlite3_open(dest.c_str(), &db_copy);
    if (res != SQLITE_OK) {
        sqlite3_close(db_copy);
        return false;
    }
    sqlite3_backup* backup = sqlite3_backup_init(db_copy, "main", m_db, "main");
    if (!backup) {
        sqlite3_backup_finish(backup);
        sqlite3_close(db_copy);
        return false;
    }
    // Copy all of the pages using -1
    res = sqlite3_backup_step(backup, -1);
    if (res != SQLITE_DONE) {
        sqlite3_backup_finish(backup);
        sqlite3_close(db_copy);
        return false;
    }
    res = sqlite3_backup_finish(backup);
    sqlite3_close(db_copy);
    return res == SQLITE_OK;
}

void SQLiteDatabase::Close()
{
    if (!m_db) return;

    assert(m_refcount == 0);

    // Free all of the prepared statements
    sqlite3_finalize(m_read_stmt);
    sqlite3_finalize(m_insert_stmt);
    sqlite3_finalize(m_overwrite_stmt);
    sqlite3_finalize(m_delete_stmt);
    sqlite3_finalize(m_cursor_stmt);

    int res = sqlite3_close(m_db);
    if (res != SQLITE_OK) {
        throw std::runtime_error(strprintf("SQLiteDatabase: Failed to close database: %s\n", sqlite3_errstr(res)));
    }
    m_db = nullptr;

    UnlockDirectory(m_dir_path, ".walletlock");
}

void SQLiteDatabase::Flush() {}

void SQLiteDatabase::ReloadDbEnv() {}

void SQLiteDatabase::RemoveRef()
{
    m_refcount--;
}

void SQLiteDatabase::AddRef()
{
    m_refcount++;
}

std::unique_ptr<DatabaseBatch> SQLiteDatabase::MakeBatch(const char* mode, bool flush_on_close)
{
    return nullptr;
}

SQLiteBatch::SQLiteBatch(SQLiteDatabase& database, const char* mode)
    : m_database(database)
{
    m_read_only = (!strchr(mode, '+') && !strchr(mode, 'w'));
    m_database.AddRef();
    m_database.Open(mode);
}

void SQLiteBatch::Flush() {}

void SQLiteBatch::Close()
{
    if (m_database.m_db && sqlite3_get_autocommit(m_database.m_db) == 0) {
        TxnAbort();
    }
    m_database.RemoveRef();
}

bool SQLiteBatch::ReadKey(CDataStream&& key, CDataStream& value)
{
    if (!m_database.m_db) return false;
    assert(m_database.m_read_stmt);

    // Bind: leftmost parameter in statement is index 1
    int res = sqlite3_bind_blob(m_database.m_read_stmt, 1, key.data(), key.size(), SQLITE_STATIC);
    if (res != SQLITE_OK) {
        sqlite3_clear_bindings(m_database.m_read_stmt);
        sqlite3_reset(m_database.m_read_stmt);
        return false;
    }
    res = sqlite3_step(m_database.m_read_stmt);
    if (res != SQLITE_ROW) {
        sqlite3_clear_bindings(m_database.m_read_stmt);
        sqlite3_reset(m_database.m_read_stmt);
        return false;
    }
    // Leftmost column in result is index 0
    const char* data = (const char*)sqlite3_column_blob(m_database.m_read_stmt, 0);
    int data_size = sqlite3_column_bytes(m_database.m_read_stmt, 0);
    value.write(data, data_size);

    sqlite3_clear_bindings(m_database.m_read_stmt);
    sqlite3_reset(m_database.m_read_stmt);
    return true;
}

bool SQLiteBatch::WriteKey(CDataStream&& key, CDataStream&& value, bool overwrite)
{
    if (!m_database.m_db) return false;
    if (m_read_only) throw std::runtime_error("Write called on database in read-only mode");
    assert(m_database.m_insert_stmt && m_database.m_overwrite_stmt);

    sqlite3_stmt* stmt;
    if (overwrite) {
        stmt = m_database.m_overwrite_stmt;
    } else {
        stmt = m_database.m_insert_stmt;
    }

    // Bind: leftmost parameter in statement is index 1
    // Insert index 1 is key, 2 is value
    int res = sqlite3_bind_blob(stmt, 1, key.data(), key.size(), SQLITE_STATIC);
    if (res != SQLITE_OK) {
        sqlite3_clear_bindings(stmt);
        sqlite3_reset(stmt);
        return false;
    }
    res = sqlite3_bind_blob(stmt, 2, value.data(), value.size(), SQLITE_STATIC);
    if (res != SQLITE_OK) {
        sqlite3_clear_bindings(stmt);
        sqlite3_reset(stmt);
        return false;
    }

    // Execute
    res = sqlite3_step(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_reset(stmt);
    return res == SQLITE_DONE;
}

bool SQLiteBatch::EraseKey(CDataStream&& key)
{
    if (!m_database.m_db) return false;
    if (m_read_only) throw std::runtime_error("Erase called on database in read-only mode");
    assert(m_database.m_delete_stmt);

    // Bind: leftmost parameter in statement is index 1
    int res = sqlite3_bind_blob(m_database.m_delete_stmt, 1, key.data(), key.size(), SQLITE_STATIC);
    if (res != SQLITE_OK) {
        sqlite3_clear_bindings(m_database.m_delete_stmt);
        sqlite3_reset(m_database.m_delete_stmt);
        return false;
    }

    // Execute
    res = sqlite3_step(m_database.m_delete_stmt);
    sqlite3_clear_bindings(m_database.m_delete_stmt);
    sqlite3_reset(m_database.m_delete_stmt);
    return res == SQLITE_DONE;
}

bool SQLiteBatch::HasKey(CDataStream&& key)
{
    if (!m_database.m_db) return false;
    assert(m_database.m_read_stmt);

    // Bind: leftmost parameter in statement is index 1
    bool ret = false;
    int res = sqlite3_bind_blob(m_database.m_read_stmt, 1, key.data(), key.size(), SQLITE_STATIC);
    if (res == SQLITE_OK) {
        res = sqlite3_step(m_database.m_read_stmt);
        if (res == SQLITE_ROW) {
            ret = true;
        }
    }

    sqlite3_clear_bindings(m_database.m_read_stmt);
    sqlite3_reset(m_database.m_read_stmt);
    return ret;
}

bool SQLiteBatch::StartCursor()
{
    assert(!m_cursor_init);
    if (!m_database.m_db) return false;
    m_cursor_init = true;
    return true;
}

bool SQLiteBatch::ReadAtCursor(CDataStream& key, CDataStream& value, bool& complete)
{
    complete = false;

    if (!m_cursor_init) return false;

    int res = sqlite3_step(m_database.m_cursor_stmt);
    if (res == SQLITE_DONE) {
        complete = true;
        return true;
    } else if (res != SQLITE_ROW) {
        return false;
    }

    // Leftmost column in result is index 0
    const char* key_data = (const char*)sqlite3_column_blob(m_database.m_cursor_stmt, 0);
    int key_data_size = sqlite3_column_bytes(m_database.m_cursor_stmt, 0);
    key.write(key_data, key_data_size);
    const char* value_data = (const char*)sqlite3_column_blob(m_database.m_cursor_stmt, 1);
    int value_data_size = sqlite3_column_bytes(m_database.m_cursor_stmt, 1);
    value.write(value_data, value_data_size);
    return true;
}

void SQLiteBatch::CloseCursor()
{
    sqlite3_reset(m_database.m_cursor_stmt);
    m_cursor_init = false;
}

bool SQLiteBatch::TxnBegin()
{
    if (!m_database.m_db || sqlite3_get_autocommit(m_database.m_db) == 0) return false;
    int res = sqlite3_exec(m_database.m_db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
    return res == SQLITE_OK;
}

bool SQLiteBatch::TxnCommit()
{
    if (!m_database.m_db || sqlite3_get_autocommit(m_database.m_db) != 0) return false;
    int res = sqlite3_exec(m_database.m_db, "COMMIT TRANSACTION", nullptr, nullptr, nullptr);
    return res == SQLITE_OK;
}

bool SQLiteBatch::TxnAbort()
{
    if (!m_database.m_db || sqlite3_get_autocommit(m_database.m_db) != 0) return false;
    int res = sqlite3_exec(m_database.m_db, "ROLLBACK TRANSACTION", nullptr, nullptr, nullptr);
    return res == SQLITE_OK;
}

bool ExistsSQLiteDatabase(const fs::path& path)
{
    return false;
}

std::unique_ptr<SQLiteDatabase> MakeSQLiteDatabase(const fs::path& path, const DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error)
{
    fs::path file = path / DATABASE_FILENAME;
    auto db = MakeUnique<SQLiteDatabase>(path, file);
    if (options.verify && fs::is_regular_file(file) && !db->Verify(error)) {
        status = DatabaseStatus::FAILED_VERIFY;
        return nullptr;
    }
    return db;
}

std::string SQLiteDatabaseVersion()
{
    return std::string(sqlite3_libversion());
}
