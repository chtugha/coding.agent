#pragma once

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#  include <Security/Security.h>
#  include <mach-o/dyld.h>
#endif

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/rand.h>
#include <openssl/err.h>

namespace prodigy_tls {

static inline std::string get_exe_dir() {
#ifdef __APPLE__
    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        char* last_slash = strrchr(path, '/');
        if (last_slash) { *last_slash = '\0'; return std::string(path); }
    }
#endif
    return ".";
}

static inline std::string tls_dir() {
    return get_exe_dir() + "/tls";
}

static inline bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static inline bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return !out.empty();
}

static inline bool generate_self_signed_cert(const std::string& cert_path,
                                             const std::string& key_path) {
    EVP_PKEY* pkey = EVP_PKEY_new();
    if (!pkey) return false;

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!ctx) { EVP_PKEY_free(pkey); return false; }

    if (EVP_PKEY_keygen_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) <= 0 ||
        EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return false;
    }
    EVP_PKEY_CTX_free(ctx);

    X509* x509 = X509_new();
    if (!x509) { EVP_PKEY_free(pkey); return false; }

    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 365L * 24 * 60 * 60 * 10);
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        (const unsigned char*)"Prodigy Local Service", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
        (const unsigned char*)"Prodigy", -1, -1, 0);
    X509_set_issuer_name(x509, name);

    X509V3_CTX v3ctx;
    X509V3_set_ctx_nodb(&v3ctx);
    X509V3_set_ctx(&v3ctx, x509, x509, nullptr, nullptr, 0);
    X509_EXTENSION* san_ext = X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_subject_alt_name,
        const_cast<char*>("DNS:localhost,IP:127.0.0.1"));
    if (san_ext) {
        X509_add_ext(x509, san_ext, -1);
        X509_EXTENSION_free(san_ext);
    }

    X509_sign(x509, pkey, EVP_sha256());

    FILE* f_key = fopen(key_path.c_str(), "w");
    if (!f_key) { X509_free(x509); EVP_PKEY_free(pkey); return false; }
    PEM_write_PrivateKey(f_key, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    fclose(f_key);
    chmod(key_path.c_str(), 0600);

    FILE* f_cert = fopen(cert_path.c_str(), "w");
    if (!f_cert) { X509_free(x509); EVP_PKEY_free(pkey); return false; }
    PEM_write_X509(f_cert, x509);
    fclose(f_cert);

    X509_free(x509);
    EVP_PKEY_free(pkey);

    std::fprintf(stderr, "[tls_cert] Generated self-signed TLS certificate: %s\n",
                 cert_path.c_str());
    return true;
}

struct CertData {
    std::string cert_pem;
    std::string key_pem;
};

static std::once_flag g_cert_once;
static CertData       g_cert_data;

static inline const CertData& ensure_certs() {
    std::call_once(g_cert_once, []() {
        std::string dir = tls_dir();
        mkdir(dir.c_str(), 0700);

        std::string cert_path = dir + "/server.crt";
        std::string key_path  = dir + "/server.key";

        if (!file_exists(cert_path) || !file_exists(key_path)) {
            if (!generate_self_signed_cert(cert_path, key_path)) {
                std::fprintf(stderr, "[tls_cert] FATAL: cannot generate TLS certificate\n");
                return;
            }
        }

        if (!read_file(cert_path, g_cert_data.cert_pem) ||
            !read_file(key_path, g_cert_data.key_pem)) {
            std::fprintf(stderr, "[tls_cert] FATAL: cannot read TLS certificate files\n");
        } else {
            std::fprintf(stderr, "[tls_cert] Loaded TLS certificate from %s\n",
                         cert_path.c_str());
        }
    });
    return g_cert_data;
}

static const char* IC_KEYCHAIN_SERVICE = "com.prodigy.interconnect_encryption";
static const char* IC_KEYCHAIN_ACCOUNT = "ic_key";

static std::string    g_ic_key;
static std::once_flag g_ic_key_once;

static inline void secure_zero_tls(void* ptr, std::size_t len) noexcept {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
    while (len--) *p++ = 0;
}

#ifdef __APPLE__
static inline std::string ic_keychain_load() {
    CFStringRef cfSvc = CFStringCreateWithCString(nullptr, IC_KEYCHAIN_SERVICE, kCFStringEncodingUTF8);
    CFStringRef cfAcc = CFStringCreateWithCString(nullptr, IC_KEYCHAIN_ACCOUNT, kCFStringEncodingUTF8);

    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(q, kSecClass,      kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, cfSvc);
    CFDictionarySetValue(q, kSecAttrAccount, cfAcc);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);

    CFDataRef data = nullptr;
    OSStatus  st   = SecItemCopyMatching(q, reinterpret_cast<CFTypeRef*>(&data));
    CFRelease(q); CFRelease(cfSvc); CFRelease(cfAcc);

    if (st == errSecSuccess && data) {
        std::string key(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                        static_cast<std::size_t>(CFDataGetLength(data)));
        CFRelease(data);
        return key;
    }
    if (data) CFRelease(data);
    return {};
}

static inline bool ic_keychain_store(const std::string& key) {
    CFStringRef cfSvc  = CFStringCreateWithCString(nullptr, IC_KEYCHAIN_SERVICE, kCFStringEncodingUTF8);
    CFStringRef cfAcc  = CFStringCreateWithCString(nullptr, IC_KEYCHAIN_ACCOUNT, kCFStringEncodingUTF8);
    CFDataRef   cfData = CFDataCreate(nullptr,
                             reinterpret_cast<const UInt8*>(key.data()),
                             static_cast<CFIndex>(key.size()));

    CFMutableDictionaryRef addQ = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(addQ, kSecClass,         kSecClassGenericPassword);
    CFDictionarySetValue(addQ, kSecAttrService,   cfSvc);
    CFDictionarySetValue(addQ, kSecAttrAccount,   cfAcc);
    CFDictionarySetValue(addQ, kSecValueData,     cfData);
    CFDictionarySetValue(addQ, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);

    OSStatus st = SecItemAdd(addQ, nullptr);
    CFRelease(addQ); CFRelease(cfSvc); CFRelease(cfAcc); CFRelease(cfData);

    if (st == errSecSuccess) return true;
    if (st == errSecDuplicateItem) {
        CFStringRef cfSvc2  = CFStringCreateWithCString(nullptr, IC_KEYCHAIN_SERVICE, kCFStringEncodingUTF8);
        CFStringRef cfAcc2  = CFStringCreateWithCString(nullptr, IC_KEYCHAIN_ACCOUNT, kCFStringEncodingUTF8);
        CFDataRef   cfData2 = CFDataCreate(nullptr,
                                 reinterpret_cast<const UInt8*>(key.data()),
                                 static_cast<CFIndex>(key.size()));

        CFMutableDictionaryRef findQ = CFDictionaryCreateMutable(
            nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(findQ, kSecClass,       kSecClassGenericPassword);
        CFDictionarySetValue(findQ, kSecAttrService, cfSvc2);
        CFDictionarySetValue(findQ, kSecAttrAccount, cfAcc2);

        CFMutableDictionaryRef updQ = CFDictionaryCreateMutable(
            nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(updQ, kSecValueData, cfData2);

        st = SecItemUpdate(findQ, updQ);
        CFRelease(findQ); CFRelease(updQ);
        CFRelease(cfSvc2); CFRelease(cfAcc2); CFRelease(cfData2);
        return st == errSecSuccess;
    }
    return false;
}
#endif

static inline const std::string& get_interconnect_key() {
    std::call_once(g_ic_key_once, []() {
#ifdef __APPLE__
        g_ic_key = ic_keychain_load();
        if (!g_ic_key.empty() && g_ic_key.size() == 32) {
            std::fprintf(stderr, "[ic_key] Loaded interconnect AES-256 key from macOS Keychain\n");
            return;
        }
        unsigned char raw[32];
        arc4random_buf(raw, sizeof(raw));
        g_ic_key.assign(reinterpret_cast<char*>(raw), 32);
        secure_zero_tls(raw, sizeof(raw));
        if (ic_keychain_store(g_ic_key)) {
            std::fprintf(stderr, "[ic_key] Generated new interconnect AES-256 key "
                         "and stored in macOS Keychain\n");
        } else {
            std::fprintf(stderr, "[ic_key] Warning: interconnect key Keychain storage failed\n");
        }
#else
        g_ic_key.resize(32, '\0');
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&g_ic_key[0]), 32) != 1) {
            std::fprintf(stderr, "[ic_key] FATAL: RAND_bytes failed for interconnect key\n");
        }
#endif
    });
    return g_ic_key;
}

static constexpr int IC_GCM_IV_LEN  = 12;
static constexpr int IC_GCM_TAG_LEN = 16;

static inline bool ic_encrypt(const uint8_t* plaintext, size_t plain_len,
                               uint8_t* ciphertext, size_t& cipher_len) {
    const std::string& key = get_interconnect_key();
    if (key.size() != 32) return false;

    uint8_t iv[IC_GCM_IV_LEN];
    if (RAND_bytes(iv, IC_GCM_IV_LEN) != 1) return false;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    int len = 0;
    bool ok = true;
    ok = ok && (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1);
    ok = ok && (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IC_GCM_IV_LEN, nullptr) == 1);
    ok = ok && (EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                    reinterpret_cast<const unsigned char*>(key.data()), iv) == 1);
    ok = ok && (EVP_EncryptUpdate(ctx, ciphertext + IC_GCM_IV_LEN,
                    &len, plaintext, static_cast<int>(plain_len)) == 1);
    int clen = len;
    ok = ok && (EVP_EncryptFinal_ex(ctx, ciphertext + IC_GCM_IV_LEN + clen, &len) == 1);
    clen += len;
    ok = ok && (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, IC_GCM_TAG_LEN,
                    ciphertext + IC_GCM_IV_LEN + clen) == 1);

    memcpy(ciphertext, iv, IC_GCM_IV_LEN);
    cipher_len = IC_GCM_IV_LEN + clen + IC_GCM_TAG_LEN;

    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static inline bool ic_decrypt(const uint8_t* ciphertext, size_t cipher_len,
                               uint8_t* plaintext, size_t& plain_len) {
    const std::string& key = get_interconnect_key();
    if (key.size() != 32) return false;
    if (cipher_len < (size_t)(IC_GCM_IV_LEN + IC_GCM_TAG_LEN)) return false;

    const uint8_t* iv = ciphertext;
    size_t enc_len = cipher_len - IC_GCM_IV_LEN - IC_GCM_TAG_LEN;
    const uint8_t* enc_data = ciphertext + IC_GCM_IV_LEN;
    const uint8_t* tag = ciphertext + IC_GCM_IV_LEN + enc_len;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    int len = 0;
    bool ok = true;
    ok = ok && (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1);
    ok = ok && (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IC_GCM_IV_LEN, nullptr) == 1);
    ok = ok && (EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                    reinterpret_cast<const unsigned char*>(key.data()), iv) == 1);
    ok = ok && (EVP_DecryptUpdate(ctx, plaintext, &len,
                    enc_data, static_cast<int>(enc_len)) == 1);
    int plen = len;
    ok = ok && (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, IC_GCM_TAG_LEN,
                    const_cast<uint8_t*>(tag)) == 1);
    ok = ok && (EVP_DecryptFinal_ex(ctx, plaintext + plen, &len) == 1);
    plen += len;

    plain_len = static_cast<size_t>(plen);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static inline size_t ic_encrypted_size(size_t plain_len) {
    return IC_GCM_IV_LEN + plain_len + IC_GCM_TAG_LEN;
}

}
