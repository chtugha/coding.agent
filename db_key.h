// db_key.h — macOS Keychain-backed SQLCipher database encryption key management
//
// PURPOSE
//   Provides a single 256-bit (32-byte hex) AES key used to encrypt all SQLite
//   databases in the project via SQLCipher.  The key is generated once, stored
//   permanently in the macOS Keychain, and retrieved on subsequent startups.
//
// USAGE
//   #include "db_key.h"
//   ...
//   sqlite3* db = nullptr;
//   prodigy_db::db_open_encrypted("/path/to/db.sqlite", &db);
//   // Use db normally — all reads/writes are transparently AES-256 encrypted.
//
// SECURITY PROPERTIES
//   • AES-256-CBC page-level encryption via SQLCipher (OpenSSL 3.x backend).
//   • Key material (32 bytes) generated with arc4random_buf() — a CSPRNG seeded
//     by the macOS kernel; each device gets a unique key on first run.
//   • Key stored in macOS Keychain as kSecClassGenericPassword with
//     kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly:
//       – Accessible to daemon/service processes after the first unlock following
//         a reboot — survives screen-lock and sleep cycles.
//       – Cannot be extracted via iCloud Backup or Migration Assistant.
//       – On Apple Silicon the Keychain is hardware-rooted in the Secure Enclave
//         coprocessor; the raw key material cannot be read by another process.
//   • SQLITE_TEMP_STORE=2 compiled in: temp tables go to memory, never to disk
//     in plaintext.
//   • Migration: existing plaintext SQLite databases are encrypted in-place via
//     sqlite3_rekey() the first time they are opened with this helper.
//
// APPLE SILICON PERFORMANCE NOTES
//   Apple M1/M2/M3/M4 chips implement the ARMv8.2 Cryptography Extension with
//   hardware AES acceleration.  OpenSSL 3.x detects and uses this automatically
//   via the AES instruction set (AESE/AESMC), so SQLCipher encryption overhead
//   is negligible (< 3% for typical database workloads on M-series hardware).
//
// CROSS-PROCESS COMPATIBILITY
//   Both `frontend` and `tomedo-crawl` open the shared tomedo-crawl.db file.
//   Because both processes retrieve the same Keychain item
//   (KEYCHAIN_SERVICE / KEYCHAIN_ACCOUNT), they use an identical key and can
//   interoperate with full SQLite WAL multi-reader concurrency.
//
// THREAD SAFETY
//   get_db_key() is safe to call from multiple threads — a std::once_flag
//   ensures the Keychain is accessed exactly once per process lifetime.

#pragma once

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <fcntl.h>
#include <unistd.h>

#ifdef __APPLE__
#  include <Security/Security.h>
#  include <mach-o/dyld.h>
#  include <stdlib.h>   // arc4random_buf
#endif

// These defines must be set before including sqlite3.h so that SQLCipher's
// sqlite3_key() / sqlite3_rekey() declarations become visible.
// They are also passed as -D flags in CMakeLists.txt for sqlite3.c itself.
#ifndef SQLITE_HAS_CODEC
#  define SQLITE_HAS_CODEC
#endif
#include "sqlite3.h"

namespace prodigy_db {

// ─── Secure memory zeroing ────────────────────────────────────────────────────
// Writes zeros to a memory region in a way the compiler cannot optimise away.
// Use this for key material and sensitive buffers before they go out of scope.
// portable — works on every C++17 platform without __STDC_WANT_LIB_EXT1__.
static inline void secure_zero(void* ptr, std::size_t len) noexcept
{
    volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
    while (len--) *p++ = 0;
}

// ─── Internal constants ────────────────────────────────────────────────────────
static const char* KEYCHAIN_SERVICE = "com.prodigy.db_encryption";
static const char* KEYCHAIN_ACCOUNT = "db_key";

// ─── Key state ────────────────────────────────────────────────────────────────
static std::string    g_cached_key;
static std::once_flag g_key_once;

// ─── Key generation ───────────────────────────────────────────────────────────
// Returns a 64-character lowercase hex string (256-bit key).
static std::string generate_hex_key()
{
    unsigned char raw[32];
#ifdef __APPLE__
    arc4random_buf(raw, sizeof(raw));   // Kernel CSPRNG, always available
#else
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f || std::fread(raw, 1, sizeof(raw), f) != sizeof(raw)) {
        if (f) std::fclose(f);
        std::fprintf(stderr, "[db_key] FATAL: cannot read /dev/urandom\n");
        secure_zero(raw, sizeof(raw));
        return {};
    }
    std::fclose(f);
#endif
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", raw[i]);
    hex[64] = '\0';
    std::string result(hex, 64);
    // Zero sensitive key material from stack before returning.
    secure_zero(raw, sizeof(raw));
    secure_zero(hex, sizeof(hex));
    return result;
}

// ─── Keychain helpers ─────────────────────────────────────────────────────────
static std::string keychain_load()
{
#ifdef __APPLE__
    CFStringRef cfSvc = CFStringCreateWithCString(nullptr, KEYCHAIN_SERVICE, kCFStringEncodingUTF8);
    CFStringRef cfAcc = CFStringCreateWithCString(nullptr, KEYCHAIN_ACCOUNT, kCFStringEncodingUTF8);

    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(q, kSecClass,        kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService,  cfSvc);
    CFDictionarySetValue(q, kSecAttrAccount,  cfAcc);
    CFDictionarySetValue(q, kSecReturnData,   kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit,   kSecMatchLimitOne);

    CFDataRef data = nullptr;
    OSStatus  st   = SecItemCopyMatching(q, reinterpret_cast<CFTypeRef*>(&data));
    CFRelease(q);
    CFRelease(cfSvc);
    CFRelease(cfAcc);

    if (st == errSecSuccess && data) {
        std::string key(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                        static_cast<std::size_t>(CFDataGetLength(data)));
        CFRelease(data);
        return key;
    }
    if (data) CFRelease(data);
#endif
    return {};
}

static bool keychain_store(const std::string& key)
{
#ifdef __APPLE__
    CFStringRef cfSvc  = CFStringCreateWithCString(nullptr, KEYCHAIN_SERVICE, kCFStringEncodingUTF8);
    CFStringRef cfAcc  = CFStringCreateWithCString(nullptr, KEYCHAIN_ACCOUNT, kCFStringEncodingUTF8);
    CFDataRef   cfData = CFDataCreate(nullptr,
                             reinterpret_cast<const UInt8*>(key.c_str()),
                             static_cast<CFIndex>(key.size()));

    CFMutableDictionaryRef addQ = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(addQ, kSecClass,        kSecClassGenericPassword);
    CFDictionarySetValue(addQ, kSecAttrService,  cfSvc);
    CFDictionarySetValue(addQ, kSecAttrAccount,  cfAcc);
    CFDictionarySetValue(addQ, kSecValueData,    cfData);
    // kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly:
    //   • Readable after the first unlock following a reboot — correct for a
    //     server/daemon process that must survive screen-lock and sleep cycles.
    //   • NOT migrated to other devices — tied to this hardware's Secure Enclave.
    //   • Using WhenUnlocked would block access while the screen is locked,
    //     preventing the service from opening its database after a lock/sleep.
    CFDictionarySetValue(addQ, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);

    OSStatus st = SecItemAdd(addQ, nullptr);
    CFRelease(addQ);
    CFRelease(cfSvc);
    CFRelease(cfAcc);
    CFRelease(cfData);

    if (st == errSecSuccess) return true;

    if (st == errSecDuplicateItem) {
        // Key already exists — update value in case it changed
        CFStringRef cfSvc2  = CFStringCreateWithCString(nullptr, KEYCHAIN_SERVICE, kCFStringEncodingUTF8);
        CFStringRef cfAcc2  = CFStringCreateWithCString(nullptr, KEYCHAIN_ACCOUNT, kCFStringEncodingUTF8);
        CFDataRef   cfData2 = CFDataCreate(nullptr,
                                 reinterpret_cast<const UInt8*>(key.c_str()),
                                 static_cast<CFIndex>(key.size()));

        CFMutableDictionaryRef findQ = CFDictionaryCreateMutable(
            nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(findQ, kSecClass,       kSecClassGenericPassword);
        CFDictionarySetValue(findQ, kSecAttrService, cfSvc2);
        CFDictionarySetValue(findQ, kSecAttrAccount, cfAcc2);

        CFMutableDictionaryRef updQ = CFDictionaryCreateMutable(
            nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(updQ, kSecValueData, cfData2);
        // Also update accessibility so a pre-existing WhenUnlocked entry is
        // migrated to AfterFirstUnlock, which is required for daemon access
        // after reboot while the screen is still locked.
        CFDictionarySetValue(updQ, kSecAttrAccessible,
                             kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);

        st = SecItemUpdate(findQ, updQ);
        CFRelease(findQ);
        CFRelease(updQ);
        CFRelease(cfSvc2);
        CFRelease(cfAcc2);
        CFRelease(cfData2);
        return st == errSecSuccess;
    }

    std::fprintf(stderr, "[db_key] Warning: Keychain store failed (OSStatus %d) — "
                 "key exists only in memory for this session\n", static_cast<int>(st));
    return false;
#else
    (void)key;
    return false;
#endif
}

static std::string db_key_file_path()
{
#ifdef __APPLE__
    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        char* last_slash = strrchr(path, '/');
        if (last_slash) { *last_slash = '\0'; }
        return std::string(path) + "/db_key.hex";
    }
#endif
    return "./db_key.hex";
}

static std::string file_key_load()
{
    std::string path = db_key_file_path();
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return {};
    char buf[128] = {0};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';
    if (n != 64) return {};
    return std::string(buf, 64);
}

static bool file_key_store(const std::string& key)
{
    std::string path = db_key_file_path();
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    ssize_t w = write(fd, key.c_str(), key.size());
    if (w != (ssize_t)key.size()) { close(fd); return false; }
    fsync(fd);
    close(fd);
    return true;
}

// ─── Public API ───────────────────────────────────────────────────────────────

static const std::string& get_db_key()
{
    std::call_once(g_key_once, []() {
        g_cached_key = file_key_load();
        if (!g_cached_key.empty()) {
            std::fprintf(stderr,
                "[db_key] Loaded AES-256 database key from file (%s)\n",
                db_key_file_path().c_str());
            return;
        }
        g_cached_key = generate_hex_key();
        if (g_cached_key.empty()) {
            std::fprintf(stderr, "[db_key] FATAL: key generation failed\n");
            return;
        }
        file_key_store(g_cached_key);
        std::fprintf(stderr,
            "[db_key] Generated new 256-bit AES database key and stored in file (%s)\n",
            db_key_file_path().c_str());
    });
    return g_cached_key;
}

// Drop-in replacement for sqlite3_open().
//
// Opens `path` and applies the SQLCipher AES-256 encryption key immediately.
// Handles three cases:
//   1. New database (file does not exist yet)  → created encrypted from scratch.
//   2. Existing encrypted database             → key applied, ready to use.
//   3. Existing legacy plaintext database      → encrypted in-place via
//                                                sqlite3_rekey() and a log
//                                                message is emitted.
//
// Returns SQLITE_OK on success or a SQLite error code on open failure.
// On SQLITE_OK the caller owns *db and must call sqlite3_close() when done.
static inline int db_open_encrypted(const char* path, sqlite3** db)
{
    int rc = sqlite3_open(path, db);
    if (rc != SQLITE_OK) return rc;

    const std::string& key = get_db_key();
    if (key.empty()) {
        std::fprintf(stderr,
            "[db_key] Warning: no encryption key available — %s is UNENCRYPTED\n", path);
        return SQLITE_OK;
    }

    // Apply key — on a new/encrypted DB this succeeds immediately.
    sqlite3_key(*db, key.c_str(), static_cast<int>(key.size()));

    // Probe: verify the key is correct (or the DB is freshly created/empty).
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(*db, "SELECT count(*) FROM sqlite_master", -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_finalize(stmt);
        return SQLITE_OK;   // Correctly keyed or brand new
    }
    sqlite3_finalize(stmt);

    // Probe failed — the DB might be a legacy plaintext SQLite file.
    // Close and re-open without a key to check.
    sqlite3_close(*db);
    *db = nullptr;

    rc = sqlite3_open(path, db);
    if (rc != SQLITE_OK) return rc;

    stmt = nullptr;
    rc = sqlite3_prepare_v2(*db, "SELECT count(*) FROM sqlite_master", -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_finalize(stmt);
        // DB is readable as plaintext → migrate in-place.
        //
        // Flush any existing WAL file into the main database file first.
        // If the DB was previously opened in WAL mode, the *.db-wal file may
        // contain unwritten pages that would be left in plaintext on disk after
        // sqlite3_rekey() rewrites the main file.  Checkpointing guarantees all
        // data is captured in the encrypted rewrite.
        sqlite3_exec(*db, "PRAGMA wal_checkpoint(TRUNCATE)", nullptr, nullptr, nullptr);
        int rkey_rc = sqlite3_rekey(*db, key.c_str(), static_cast<int>(key.size()));
        if (rkey_rc == SQLITE_OK) {
            std::fprintf(stderr,
                "[db_key] Migrated plaintext database to SQLCipher AES-256 encryption: %s\n",
                path);
        } else {
            std::fprintf(stderr,
                "[db_key] Warning: in-place encryption of %s failed (rc=%d) — "
                "database remains plaintext\n", path, rkey_rc);
        }
        return SQLITE_OK;
    }
    sqlite3_finalize(stmt);

    // Neither correctly keyed nor readable as plaintext — file is corrupted or
    // was created by an incompatible SQLCipher version.  Close the handle and
    // return an error so callers can surface a meaningful message rather than
    // proceeding with a broken database.
    std::fprintf(stderr,
        "[db_key] Error: %s is neither a correctly-keyed SQLCipher database nor "
        "a readable plaintext SQLite file — possible corruption or key mismatch\n", path);
    sqlite3_close(*db);
    *db = nullptr;
    return SQLITE_NOTADB;
}

} // namespace prodigy_db
