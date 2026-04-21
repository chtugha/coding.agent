#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#  include <mach-o/dyld.h>
#endif

#include <openssl/bio.h>
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

static inline std::string cert_file_path() {
    return tls_dir() + "/server.crt";
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

static inline bool generate_self_signed_cert_impl(const std::string& cert_path,
                                                  const std::string& key_path,
                                                  long validity_seconds) {
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
    X509_gmtime_adj(X509_get_notAfter(x509), validity_seconds);
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

static inline bool generate_self_signed_cert(const std::string& cert_path,
                                             const std::string& key_path) {
    return generate_self_signed_cert_impl(cert_path, key_path, 365L * 24 * 60 * 60 * 10);
}

static inline bool generate_self_signed_cert_90d(const std::string& cert_path,
                                                  const std::string& key_path) {
    return generate_self_signed_cert_impl(cert_path, key_path, 90L * 24 * 60 * 60);
}

static inline time_t get_cert_expiry(const std::string& cert_path) {
    FILE* f = fopen(cert_path.c_str(), "r");
    if (!f) return 0;
    X509* x509 = PEM_read_X509(f, nullptr, nullptr, nullptr);
    fclose(f);
    if (!x509) return 0;

    const ASN1_TIME* not_after = X509_get0_notAfter(x509);
    struct tm tm_val{};
    int ret = ASN1_TIME_to_tm(not_after, &tm_val);
    X509_free(x509);
    if (ret != 1) return 0;
    return timegm(&tm_val);
}

static inline bool validate_pem_cert(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
    if (!bio) return false;
    X509* x = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!x) return false;
    X509_free(x);
    return true;
}

static inline bool validate_pem_key(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
    if (!bio) return false;
    EVP_PKEY* k = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!k) return false;
    EVP_PKEY_free(k);
    return true;
}

static inline std::vector<std::string> list_certs_in_dir(const std::string& dir) {
    std::vector<std::string> result;
    DIR* d = opendir(dir.c_str());
    if (!d) return result;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() > 4 && name.substr(name.size() - 4) == ".crt") {
            result.push_back(name);
        }
    }
    closedir(d);
    std::sort(result.begin(), result.end());
    return result;
}

struct CertData {
    std::string cert_pem;
    std::string key_pem;
};

static std::mutex              g_cert_mutex;
static std::condition_variable g_cert_cv;
static CertData                g_cert_data;
static bool                    g_cert_loaded     = false;
static bool                    g_cert_generating = false;

static inline bool reload_certs(const std::string& cert_path, const std::string& key_path) {
    CertData tmp;
    if (!read_file(cert_path, tmp.cert_pem) || !read_file(key_path, tmp.key_pem)) {
        std::fprintf(stderr, "[tls_cert] Cannot read cert/key: %s / %s\n",
                     cert_path.c_str(), key_path.c_str());
        return false;
    }
    std::lock_guard<std::mutex> lk(g_cert_mutex);
    g_cert_data = std::move(tmp);
    g_cert_loaded = true;
    std::fprintf(stderr, "[tls_cert] Loaded TLS certificate from %s\n", cert_path.c_str());
    return true;
}

static inline const CertData& ensure_certs() {
    std::unique_lock<std::mutex> lk(g_cert_mutex);
    g_cert_cv.wait(lk, []{ return g_cert_loaded || !g_cert_generating; });
    if (g_cert_loaded) return g_cert_data;
    g_cert_generating = true;
    lk.unlock();

    std::string dir = tls_dir();
    mkdir(dir.c_str(), 0700);

    std::string cert_path = dir + "/server.crt";
    std::string key_path  = dir + "/server.key";

    bool ok = true;
    if (!file_exists(cert_path) || !file_exists(key_path)) {
        if (!generate_self_signed_cert_90d(cert_path, key_path)) {
            std::fprintf(stderr, "[tls_cert] FATAL: cannot generate TLS certificate\n");
            ok = false;
        }
    }

    if (ok) reload_certs(cert_path, key_path);

    lk.lock();
    g_cert_generating = false;
    lk.unlock();
    g_cert_cv.notify_all();

    lk.lock();
    return g_cert_data;
}

static inline CertData get_certs() {
    std::unique_lock<std::mutex> lk(g_cert_mutex);
    if (g_cert_loaded) return g_cert_data;
    lk.unlock();
    ensure_certs();
    lk.lock();
    return g_cert_data;
}

static std::string    g_ic_key;
static std::once_flag g_ic_key_once;

static inline std::string ic_encryption_flag_path() {
    return get_exe_dir() + "/ic_encryption.flag";
}

// Interconnect traffic encryption is OPT-IN. Default is OFF (plaintext
// framing) — especially for debug purposes on a fresh install. Operators
// enable it via the TLS Certificates tab in the web frontend, which
// writes "1" to ic_encryption.flag; disabling deletes the file or
// writes "0". Services read the flag once on startup (call_once),
// so a change requires restarting every pipeline service for the new
// setting to take effect.
static std::atomic<int> g_ic_encryption_enabled{-1};
static std::once_flag   g_ic_encryption_once;

static inline bool ic_encryption_enabled() {
    std::call_once(g_ic_encryption_once, []() {
        std::string path = ic_encryption_flag_path();
        FILE* f = fopen(path.c_str(), "rb");
        int enabled = 0;
        if (f) {
            char buf[4] = {0};
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            if (n > 0 && buf[0] == '1') enabled = 1;
        }
        g_ic_encryption_enabled.store(enabled, std::memory_order_release);
        if (enabled) {
            std::fprintf(stderr, "[ic_key] Interconnect traffic encryption: ENABLED (AES-256-GCM)\n");
        } else {
            std::fprintf(stderr,
                "[ic_key] *** WARNING *** Interconnect traffic encryption is DISABLED (plaintext framing).\n"
                "[ic_key] *** WARNING *** Enable it in the web frontend (TLS Certificates tab) and restart all pipeline services for production deployments.\n");
        }
    });
    return g_ic_encryption_enabled.load(std::memory_order_acquire) == 1;
}

static inline bool ic_encryption_set_persistent(bool enabled) {
    std::string path = ic_encryption_flag_path();
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    const char* v = enabled ? "1" : "0";
    ssize_t n = write(fd, v, 1);
    close(fd);
    return n == 1;
}

static inline void secure_zero_tls(void* ptr, std::size_t len) noexcept {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
    while (len--) *p++ = 0;
}

static inline std::string ic_key_file_path() {
    return get_exe_dir() + "/ic_key.bin";
}

static inline std::string ic_file_load() {
    std::string path = ic_key_file_path();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    char buf[32];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n != 32) return {};
    std::string key(buf, 32);
    secure_zero_tls(buf, sizeof(buf));
    return key;
}

// Atomically create the key file with mode 0600 via O_EXCL so two racing
// processes cannot both write different keys; the loser sees EEXIST and
// falls back to reading the winner's key from disk. Permissions are set
// at creation time (no fopen→chmod TOCTOU window).
static inline bool ic_file_store(const std::string& key) {
    if (key.size() != 32) return false;
    std::string path = ic_key_file_path();
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    ssize_t written = 0;
    const char* buf = key.data();
    while (written < 32) {
        ssize_t n = write(fd, buf + written, 32 - written);
        if (n <= 0) { close(fd); unlink(path.c_str()); return false; }
        written += n;
    }
    close(fd);
    return true;
}

static inline const std::string& get_interconnect_key() {
    std::call_once(g_ic_key_once, []() {
        g_ic_key = ic_file_load();
        if (!g_ic_key.empty() && g_ic_key.size() == 32) {
            std::fprintf(stderr, "[ic_key] Loaded interconnect AES-256 key from key file\n");
            return;
        }
        unsigned char raw[32];
#ifdef __APPLE__
        arc4random_buf(raw, sizeof(raw));
#else
        if (RAND_bytes(raw, sizeof(raw)) != 1) {
            std::fprintf(stderr, "[ic_key] FATAL: RAND_bytes failed for interconnect key\n");
        }
#endif
        g_ic_key.assign(reinterpret_cast<char*>(raw), 32);
        secure_zero_tls(raw, sizeof(raw));
        if (ic_file_store(g_ic_key)) {
            std::fprintf(stderr, "[ic_key] Generated new interconnect AES-256 key and stored in key file\n");
        } else {
            g_ic_key = ic_file_load();
            if (!g_ic_key.empty() && g_ic_key.size() == 32) {
                std::fprintf(stderr, "[ic_key] Race detected — loaded winner's key from key file\n");
            } else {
                std::fprintf(stderr, "[ic_key] Warning: interconnect key file storage failed\n");
            }
        }
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
