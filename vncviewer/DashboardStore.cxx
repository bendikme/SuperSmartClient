/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>
#include "DashboardStore.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <sqlite3.h>
#ifdef HAVE_GNUTLS
#include <gnutls/crypto.h>
#endif
#ifdef WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace dashboard {
namespace {
constexpr size_t maxDatabase = 32 * 1024 * 1024;
const std::string exportMagic = "SuperSmartClient encrypted database 1\n";
using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
void checked(sqlite3* db, int code) {
  if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
    throw std::runtime_error(std::string("Layout database: ") + sqlite3_errmsg(db));
}
void execute(sqlite3* db, const char* sql) { checked(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr)); }
Database openDatabase(const std::string& path, bool writable) {
  sqlite3* pointer = nullptr;
  int result = sqlite3_open_v2(path.c_str(), &pointer,
    writable ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY, nullptr);
  Database db(pointer, sqlite3_close);
  checked(db.get(), result);
  sqlite3_busy_timeout(db.get(), 1000);
  sqlite3_limit(db.get(), SQLITE_LIMIT_LENGTH, static_cast<int>(maxDatabase));
  sqlite3_db_config(db.get(), SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
  sqlite3_db_config(db.get(), SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
  return db;
}
Statement prepare(sqlite3* db, const char* sql) {
  sqlite3_stmt* pointer = nullptr;
  checked(db, sqlite3_prepare_v2(db, sql, -1, &pointer, nullptr));
  return Statement(pointer, sqlite3_finalize);
}
void bind(sqlite3* db, sqlite3_stmt* statement, int column, const std::string& value) {
  checked(db, sqlite3_bind_text(statement, column, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
}
std::string textColumn(sqlite3_stmt* statement, int column) {
  const auto* text = sqlite3_column_text(statement, column);
  return text ? std::string(reinterpret_cast<const char*>(text), sqlite3_column_bytes(statement, column)) : "";
}
void validateLibrary(const Library& library) {
  if (library.layouts.empty() || library.layouts.size() > 100)
    throw std::runtime_error("A database must contain between 1 and 100 layouts");
  std::set<std::string> ids, names;
  size_t total = 0;
  for (const auto& layout : library.layouts) {
    validateLayoutName(layout.name);
    if (layout.id.empty() || layout.id.size() > 64 || !ids.insert(layout.id).second || !names.insert(layout.name).second)
      throw std::runtime_error("Layout names and identifiers must be unique");
    total += encodeWorkspace(layout.workspace).size();
    if (total > maxDatabase / 2) throw std::runtime_error("Layout database exceeds the size limit");
  }
  activeLayout(library);
}
void writeDatabase(sqlite3* db, const Library& library) {
  validateLibrary(library);
  execute(db, "BEGIN IMMEDIATE");
  // Closing the database rolls the transaction back if any statement fails.
  execute(db, "PRAGMA user_version=1; PRAGMA application_id=1397965636;"
    "CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS layouts (id TEXT PRIMARY KEY, name TEXT NOT NULL UNIQUE, workspace TEXT NOT NULL, position INTEGER NOT NULL);"
    "DELETE FROM layouts; DELETE FROM metadata;");
  auto meta = prepare(db, "INSERT INTO metadata VALUES ('active', ?)");
  bind(db, meta.get(), 1, library.active); checked(db, sqlite3_step(meta.get()));
  auto insert = prepare(db, "INSERT INTO layouts VALUES (?, ?, ?, ?)");
  int position = 0;
  for (const auto& layout : library.layouts) {
    sqlite3_reset(insert.get()); sqlite3_clear_bindings(insert.get());
    bind(db, insert.get(), 1, layout.id); bind(db, insert.get(), 2, layout.name);
    bind(db, insert.get(), 3, encodeWorkspace(layout.workspace));
    checked(db, sqlite3_bind_int(insert.get(), 4, position++)); checked(db, sqlite3_step(insert.get()));
  }
  execute(db, "COMMIT");
}
Library readDatabase(sqlite3* db) {
  auto version = prepare(db, "PRAGMA user_version");
  checked(db, sqlite3_step(version.get()));
  if (sqlite3_column_int(version.get(), 0) != 1) throw std::runtime_error("Unsupported layout database version");
  auto application = prepare(db, "PRAGMA application_id");
  checked(db, sqlite3_step(application.get()));
  if (sqlite3_column_int(application.get(), 0) != 1397965636)
    throw std::runtime_error("This is not a SuperSmartClient database");
  Library library;
  auto meta = prepare(db, "SELECT value FROM metadata WHERE key='active'");
  checked(db, sqlite3_step(meta.get())); library.active = textColumn(meta.get(), 0);
  auto rows = prepare(db, "SELECT id, name, workspace FROM layouts ORDER BY position");
  for (;;) {
    int code = sqlite3_step(rows.get()); checked(db, code);
    if (code == SQLITE_DONE) break;
    if (library.layouts.size() >= 100) throw std::runtime_error("Too many saved layouts");
    library.layouts.push_back({textColumn(rows.get(), 0), textColumn(rows.get(), 1),
      decodeWorkspace(textColumn(rows.get(), 2))});
  }
  validateLibrary(library);
  return library;
}
std::string readFile(const std::filesystem::path& path) {
  if (std::filesystem::file_size(path) > maxDatabase) throw std::runtime_error("Database exceeds the size limit");
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot open exported database");
  return std::string(std::istreambuf_iterator<char>(input), {});
}
void writeFile(const std::filesystem::path& path, const std::string& bytes) {
  auto temporary = path; temporary += ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("Cannot create exported database");
#ifndef WIN32
  chmod(temporary.c_str(), S_IRUSR | S_IWUSR);
#endif
  output.write(bytes.data(), bytes.size()); output.flush();
  if (!output) throw std::runtime_error("Failed to write exported database");
  output.close();
#ifdef WIN32
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("Cannot replace exported database");
#else
  std::filesystem::rename(temporary, path);
#endif
}
std::string cryptExport(const std::string& input, const std::string& password, bool encrypt) {
#ifdef HAVE_GNUTLS
  if (gnutls_global_init() < 0) throw std::runtime_error("Cannot initialize encryption");
  struct GlobalGuard { ~GlobalGuard() { gnutls_global_deinit(); } } guard;
  auto check = [](int result) { if (result < 0) throw std::runtime_error("Export password is incorrect or the database is damaged"); };
  std::string salt(16, '\0'), nonce(12, '\0'), ciphertext;
  if (encrypt) {
    check(gnutls_rnd(GNUTLS_RND_RANDOM, &salt[0], salt.size()));
    check(gnutls_rnd(GNUTLS_RND_NONCE, &nonce[0], nonce.size()));
  } else {
    if (input.size() < exportMagic.size() + 44 || input.compare(0, exportMagic.size(), exportMagic))
      throw std::runtime_error("Invalid encrypted database");
    salt = input.substr(exportMagic.size(), 16); nonce = input.substr(exportMagic.size() + 16, 12);
    ciphertext = input.substr(exportMagic.size() + 28);
  }
  unsigned char keyBytes[32]{};
  struct KeyGuard { unsigned char* key; ~KeyGuard() { gnutls_memset(key, 0, 32); } } keyGuard{keyBytes};
  gnutls_datum_t pass{reinterpret_cast<unsigned char*>(const_cast<char*>(password.data())), static_cast<unsigned>(password.size())};
  gnutls_datum_t saltData{reinterpret_cast<unsigned char*>(&salt[0]), static_cast<unsigned>(salt.size())};
  check(gnutls_pbkdf2(GNUTLS_MAC_SHA256, &pass, &saltData, 600000, keyBytes, sizeof(keyBytes)));
  gnutls_datum_t key{keyBytes, sizeof(keyBytes)};
  gnutls_aead_cipher_hd_t handle = nullptr;
  check(gnutls_aead_cipher_init(&handle, GNUTLS_CIPHER_AES_256_GCM, &key));
  struct CipherGuard { gnutls_aead_cipher_hd_t cipher; ~CipherGuard() { gnutls_aead_cipher_deinit(cipher); } } cipherGuard{handle};
  const std::string& source = encrypt ? input : ciphertext;
  std::string output(source.size() + 16, '\0'); size_t length = output.size();
  if (encrypt) check(gnutls_aead_cipher_encrypt(handle, nonce.data(), nonce.size(),
    exportMagic.data(), exportMagic.size(), 16, source.data(), source.size(), &output[0], &length));
  else check(gnutls_aead_cipher_decrypt(handle, nonce.data(), nonce.size(),
    exportMagic.data(), exportMagic.size(), 16, source.data(), source.size(), &output[0], &length));
  output.resize(length);
  return encrypt ? exportMagic + salt + nonce + output : output;
#else
  (void)input; (void)password; (void)encrypt;
  throw std::runtime_error("Password-protected export requires GnuTLS");
#endif
}
}

void validateLayoutName(const std::string& name) {
  if (name.empty() || name.size() > 100 || name.find_first_of("\r\n\t") != std::string::npos || name.find('\0') != std::string::npos)
    throw std::runtime_error("Enter a layout name (up to 100 characters)");
}
size_t activeLayout(const Library& library) {
  for (size_t index = 0; index < library.layouts.size(); ++index)
    if (library.layouts[index].id == library.active) return index;
  throw std::runtime_error("Active layout is missing from the database");
}
Library newLibrary() {
  SavedLayout layout{newId(), "Main workspace", {}};
  return {layout.id, {layout}};
}
Library loadLibrary(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) return newLibrary();
  if (std::filesystem::file_size(path) > maxDatabase) throw std::runtime_error("Database exceeds the size limit");
  auto db = openDatabase(path.u8string(), false);
  return readDatabase(db.get());
}
void saveLibrary(const std::filesystem::path& path, const Library& library) {
  validateLibrary(library);
  if (std::filesystem::exists(path) && std::filesystem::file_size(path)) {
    // Do not silently replace an unrelated or newer database.
    auto existing = openDatabase(path.u8string(), false); readDatabase(existing.get());
  }
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
  auto db = openDatabase(path.u8string(), true);
#ifndef WIN32
  chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
  writeDatabase(db.get(), library);
}
SavedLayout duplicateLayout(const SavedLayout& source, const std::string& name) {
  validateLayoutName(name);
  SavedLayout result = source; result.id = newId(); result.name = name;
  for (auto& panel : result.workspace.panels) {
    if (panel.password.empty() && panel.rememberPassword && !panel.credential.empty())
      panel.password = unprotectPassword(panel.id, panel.credential);
    panel.id = newId(); panel.credential.clear();
    if (panel.rememberPassword) panel.credential = protectPassword(panel.id, panel.password);
  }
  return result;
}
void mergeLibrary(Library& destination, const Library& imported) {
  Library result = destination;
  for (const auto& source : imported.layouts) {
    std::string name = source.name;
    auto exists = [&](const std::string& candidate) {
      return std::any_of(result.layouts.begin(), result.layouts.end(), [&](const SavedLayout& item) { return item.name == candidate; });
    };
    for (int suffix = 2; exists(name); ++suffix) name = source.name.substr(0, 80) + " (" + std::to_string(suffix) + ")";
    result.layouts.push_back(duplicateLayout(source, name));
  }
  validateLibrary(result); destination = std::move(result);
}
bool encryptedExport(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary); std::string prefix(exportMagic.size(), '\0');
  file.read(&prefix[0], prefix.size()); return prefix == exportMagic;
}
void exportLibrary(const std::filesystem::path& path, const Library& library, const std::string& password) {
  if (!password.empty() && password.size() < 8) throw std::runtime_error("Use at least 8 characters for the export password");
  Library portable = library;
  for (auto& layout : portable.layouts) for (auto& panel : layout.workspace.panels) {
    if (password.empty() || !panel.rememberPassword) panel.credential.clear();
    else {
      std::string secret = panel.password.empty() ? unprotectPassword(panel.id, panel.credential) : panel.password;
      panel.credential = "portable:" + hexEncode(secret);
    }
    panel.password.clear();
  }
  auto db = openDatabase(":memory:", true); writeDatabase(db.get(), portable);
  sqlite3_int64 length = 0;
  std::unique_ptr<unsigned char, decltype(&sqlite3_free)> bytes(sqlite3_serialize(db.get(), "main", &length, 0), sqlite3_free);
  if (!bytes || length > static_cast<sqlite3_int64>(maxDatabase)) throw std::runtime_error("Cannot serialize exported database");
  std::string output(reinterpret_cast<char*>(bytes.get()), static_cast<size_t>(length));
  if (!password.empty()) output = cryptExport(output, password, true);
  writeFile(path, output);
}
Library importLibrary(const std::filesystem::path& path, const std::string& password) {
  bool encrypted = encryptedExport(path);
  std::string bytes = readFile(path);
  if (encrypted) bytes = cryptExport(bytes, password, false);
  auto db = openDatabase(":memory:", true);
  auto* buffer = static_cast<unsigned char*>(sqlite3_malloc64(bytes.size()));
  if (!buffer) throw std::bad_alloc();
  std::memcpy(buffer, bytes.data(), bytes.size());
  int result = sqlite3_deserialize(db.get(), "main", buffer, bytes.size(), bytes.size(),
    SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_READONLY);
  // FREEONCLOSE also frees the buffer when sqlite3_deserialize itself fails.
  checked(db.get(), result);
  Library library = readDatabase(db.get());
  for (auto& layout : library.layouts) for (auto& panel : layout.workspace.panels) {
    if (encrypted && panel.credential.compare(0, 9, "portable:") == 0) {
      panel.password = hexDecode(panel.credential.substr(9));
      // Keep portable passwords in memory until mergeLibrary reseals each new
      // connection identifier using this account's password store.
      panel.id = newId();
      panel.credential.clear();
    } else {
      // Plain imports carry layout information only; foreign credential blobs
      // and keyring references must never cause access to unrelated secrets.
      panel.credential.clear(); panel.password.clear();
    }
  }
  return library;
}
}
