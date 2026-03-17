#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>

static std::string g_bin_dir;
static std::string g_models_dir;

static bool binary_exists(const std::string& name) {
    std::string path = g_bin_dir + "/" + name;
    struct stat st;
    return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IXUSR);
}

static bool dir_has_file_with_suffix(const std::string& dir, const std::string& suffix) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name(entry->d_name);
        if (name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            closedir(d);
            return true;
        }
    }
    closedir(d);
    return false;
}

static bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static pid_t launch_process(const std::string& bin, const std::vector<std::string>& args = {},
                            const std::vector<std::pair<std::string,std::string>>& env = {}) {
    pid_t pid = fork();
    if (pid == 0) {
        for (auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
        std::vector<const char*> argv;
        std::string full = g_bin_dir + "/" + bin;
        argv.push_back(full.c_str());
        for (auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
        execv(full.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    return pid;
}

static void kill_process(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    int status;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pid_t ret = waitpid(pid, &status, WNOHANG);
        if (ret == pid) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

static bool process_alive(pid_t pid) {
    if (pid <= 0) return false;
    int status;
    return waitpid(pid, &status, WNOHANG) == 0;
}

struct Pipeline {
    pid_t sip = -1;
    pid_t iap = -1;
    pid_t vad = -1;
    pid_t whisper = -1;
    pid_t llama = -1;
    pid_t kokoro = -1;
    pid_t oap = -1;
    pid_t provider = -1;

    std::vector<std::pair<std::string,std::string>> model_env() {
        return {{"WHISPERTALK_MODELS_DIR", g_models_dir}};
    }

    bool launch_provider(int sip_port, int /*duration_s*/ = 0, bool /*inject*/ = false) {
        std::vector<std::string> pargs = {
            "--port", std::to_string(sip_port)
        };
        provider = launch_process("test_sip_provider", pargs);
        if (provider <= 0) return false;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        return true;
    }

    bool launch_services(int sip_lines,
                         const std::string& sip_user,
                         const std::string& sip_server,
                         int sip_port) {
        auto env = model_env();

        sip = launch_process("sip-client",
            {"--lines", std::to_string(sip_lines), sip_user, sip_server, std::to_string(sip_port)}, env);
        if (sip <= 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        iap = launch_process("inbound-audio-processor", {}, env);
        if (iap <= 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        vad = launch_process("vad-service", {}, env);
        if (vad <= 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        whisper = launch_process("whisper-service", {}, env);
        if (whisper <= 0) return false;
        std::this_thread::sleep_for(std::chrono::seconds(5));

        llama = launch_process("llama-service", {}, env);
        if (llama <= 0) return false;
        std::this_thread::sleep_for(std::chrono::seconds(5));

        kokoro = launch_process("kokoro-service", {}, env);
        if (kokoro <= 0) return false;
        std::this_thread::sleep_for(std::chrono::seconds(5));

        oap = launch_process("outbound-audio-processor", {}, env);
        if (oap <= 0) return false;
        std::this_thread::sleep_for(std::chrono::seconds(2));

        return true;
    }

    bool all_alive() {
        return process_alive(sip) && process_alive(iap) && process_alive(vad) &&
               process_alive(whisper) && process_alive(llama) &&
               process_alive(kokoro) && process_alive(oap);
    }

    void shutdown() {
        kill_process(provider);
        kill_process(oap);
        kill_process(kokoro);
        kill_process(llama);
        kill_process(whisper);
        kill_process(vad);
        kill_process(iap);
        kill_process(sip);
        provider = sip = iap = vad = whisper = llama = kokoro = oap = -1;
    }
};

static std::string send_cmd(uint16_t port, const std::string& cmd, int timeout_s = 30) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";
    struct timeval tv{timeout_s, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(sock);
        return "";
    }
    std::string msg = cmd + "\n";
    send(sock, msg.c_str(), msg.size(), 0);
    std::string resp;
    char buf[4096];
    ssize_t n;
    while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        resp += buf;
        if (resp.find('\n') != std::string::npos) break;
    }
    close(sock);
    if (!resp.empty() && resp.back() == '\n') resp.pop_back();
    return resp;
}

static bool wait_for_port(uint16_t port, int timeout_s = 60) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(port);
        struct timeval tv{1, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            close(sock);
            return true;
        }
        close(sock);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

class RegressionTest : public ::testing::Test {
protected:
    pid_t service_pid = -1;

    void SetUp() override {
        ASSERT_FALSE(g_bin_dir.empty()) << "BIN_DIR not set";
        ASSERT_FALSE(g_models_dir.empty()) << "MODELS_DIR not set";
    }

    void TearDown() override {
        kill_process(service_pid);
        service_pid = -1;
    }
};

TEST_F(RegressionTest, LlamaSeqIdRegressionMultipleCallsAllSucceed) {
    if (!binary_exists("llama-service"))
        GTEST_SKIP() << "llama-service binary not found in " << g_bin_dir;
    if (!dir_has_file_with_suffix(g_models_dir, ".gguf"))
        GTEST_SKIP() << "No .gguf LLaMA model found in " << g_models_dir;
    std::printf("\n  === REGRESSION: llama seq_id (Bug 1) ===\n");
    std::printf("  Verifies kv_unified=true fix: all calls after the first must succeed\n");

    service_pid = launch_process("llama-service", {},
        {{"WHISPERTALK_MODELS_DIR", g_models_dir}});
    ASSERT_GT(service_pid, 0) << "Failed to launch llama-service";

    constexpr uint16_t CMD_PORT = 13132;
    ASSERT_TRUE(wait_for_port(CMD_PORT, 60)) << "llama-service cmd port not ready";

    constexpr int NUM_CALLS = 5;
    for (int i = 0; i < NUM_CALLS; i++) {
        std::string resp = send_cmd(CMD_PORT,
            "TEST_PROMPT:Hallo, wie geht es dir?", 30);
        std::printf("  Call %d response: %s\n", i + 1, resp.c_str());
        EXPECT_EQ(resp.find("RESPONSE:"), 0u)
            << "Call " << (i + 1) << " failed (expected RESPONSE:..., got: " << resp << ")";
        EXPECT_EQ(resp.find("ERROR:"), std::string::npos)
            << "Call " << (i + 1) << " returned an error";
    }
    std::printf("  =========================================\n\n");
}

TEST_F(RegressionTest, NeuTTSCodecShapeRegressionSynthesisSucceeds) {
    if (!binary_exists("neutts-service"))
        GTEST_SKIP() << "neutts-service binary not found in " << g_bin_dir;
    {
        std::string neutts_dir = g_models_dir + "/neutts-nano-german";
        if (!file_exists(neutts_dir + "/ref_codes.bin") ||
            !file_exists(neutts_dir + "/neucodec_decoder.mlmodelc"))
            GTEST_SKIP() << "NeuTTS model files not found in " << neutts_dir;
    }
    std::printf("\n  === REGRESSION: NeuTTS CoreML shape (Bug 2) ===\n");
    std::printf("  Verifies COMPILED_T=256 fix: SYNTH_WAV must return WAV_RESULT\n");

    service_pid = launch_process("neutts-service", {},
        {{"WHISPERTALK_MODELS_DIR", g_models_dir}});
    ASSERT_GT(service_pid, 0) << "Failed to launch neutts-service";

    constexpr uint16_t CMD_PORT = 13142;
    ASSERT_TRUE(wait_for_port(CMD_PORT, 120)) << "neutts-service cmd port not ready (warmup may take ~30s)";
    std::this_thread::sleep_for(std::chrono::seconds(5));  // wait for warmup synthesis to complete after port opens

    const std::vector<std::string> phrases = {
        "Guten Tag.",
        "Ja, ich höre dich.",
        "Wie kann ich Ihnen helfen?"
    };
    for (const auto& phrase : phrases) {
        std::string resp = send_cmd(CMD_PORT,
            "SYNTH_WAV:test_regression_neutts.wav|" + phrase, 60);
        std::printf("  [%s] → %s\n", phrase.c_str(), resp.c_str());
        EXPECT_EQ(resp.find("WAV_RESULT:"), 0u)
            << "Synthesis failed for [" << phrase << "]: " << resp;
        EXPECT_EQ(resp.find("ERROR:"), std::string::npos)
            << "Synthesis returned error for [" << phrase << "]: " << resp;
    }
    std::printf("  ================================================\n\n");
}

static int find_free_port() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;
    bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    socklen_t alen = sizeof(addr);
    getsockname(sock, (struct sockaddr*)&addr, &alen);
    int port = ntohs(addr.sin_port);
    close(sock);
    return port;
}

class EndToEndTest : public ::testing::Test {
protected:
    Pipeline pipeline;

    void SetUp() override {
        ASSERT_FALSE(g_bin_dir.empty()) << "BIN_DIR not set";
        ASSERT_FALSE(g_models_dir.empty()) << "MODELS_DIR not set";

        const char* required[] = {
            "test_sip_provider", "sip-client", "inbound-audio-processor",
            "vad-service", "whisper-service", "llama-service",
            "kokoro-service", "outbound-audio-processor"
        };
        for (const char* bin : required) {
            if (!binary_exists(bin))
                GTEST_SKIP() << "Pipeline binary not found: " << g_bin_dir << "/" << bin;
        }

        if (!dir_has_file_with_suffix(g_models_dir, ".gguf"))
            GTEST_SKIP() << "No .gguf LLaMA model found in " << g_models_dir;
        if (!dir_has_file_with_suffix(g_models_dir, ".bin"))
            GTEST_SKIP() << "No whisper model (.bin) found in " << g_models_dir;
        const char* required_models[] = {
            "kokoro-german/vocab.json",
            "kokoro-german/df_eva_voice.bin",
        };
        for (const char* model : required_models) {
            std::string path = g_models_dir + "/" + model;
            if (!file_exists(path))
                GTEST_SKIP() << "Model file not found: " << path;
        }
    }

    void TearDown() override {
        pipeline.shutdown();
    }
};

TEST_F(EndToEndTest, SingleCallFullPipeline) {
    int sip_port = find_free_port();

    ASSERT_TRUE(pipeline.launch_provider(sip_port, 30, true));
    ASSERT_TRUE(pipeline.launch_services(2, "test", "127.0.0.1", sip_port));

    std::printf("\n  ========== SINGLE CALL END-TO-END (real models) ==========\n");
    std::printf("  SIP provider port: %d\n", sip_port);
    std::printf("  2 lines -> 1 call -> 2 call_ids through pipeline\n");
    std::printf("  Waiting for registration + call setup...\n");

    std::this_thread::sleep_for(std::chrono::seconds(10));
    ASSERT_TRUE(pipeline.all_alive()) << "One or more services crashed during startup";

    std::printf("  All 7 services alive. Call in progress.\n");
    std::printf("  Running call for 30 seconds (with injected 400Hz tone)...\n");

    bool any_crashed = false;
    for (int i = 0; i < 30; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!pipeline.all_alive()) {
            any_crashed = true;
            std::printf("  SERVICE CRASH at t=%ds\n", i + 1);
            break;
        }
        if ((i + 1) % 10 == 0) {
            std::printf("  t=%ds - all services alive\n", i + 1);
        }
    }

    EXPECT_FALSE(any_crashed) << "Service crashed during single-call test";
    std::printf("  =========================================================\n\n");
}

TEST_F(EndToEndTest, MultipleSequentialCalls) {
    int sip_port = find_free_port();
    constexpr int CALLS = 5;
    constexpr int CALL_DURATION = 15;

    std::printf("\n  ========== %d SEQUENTIAL CALLS (real models) ==========\n", CALLS);
    std::printf("  SIP provider port: %d\n", sip_port);
    std::printf("  Each call: 2 lines, %ds duration, injected tone\n", CALL_DURATION);

    ASSERT_TRUE(pipeline.launch_provider(sip_port, CALL_DURATION, true));
    ASSERT_TRUE(pipeline.launch_services(2, "multi", "127.0.0.1", sip_port));

    std::this_thread::sleep_for(std::chrono::seconds(10));
    ASSERT_TRUE(pipeline.all_alive()) << "Services crashed during startup";

    bool any_crashed = false;
    for (int call = 0; call < CALLS; call++) {
        std::printf("  Call %d/%d in progress...\n", call + 1, CALLS);

        for (int s = 0; s < CALL_DURATION + 5; s++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!pipeline.all_alive()) {
                any_crashed = true;
                std::printf("  SERVICE CRASH during call %d at t=%ds\n", call + 1, s);
                break;
            }
        }
        if (any_crashed) break;

        if (call < CALLS - 1) {
            kill_process(pipeline.provider);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            pipeline.provider = -1;
            ASSERT_TRUE(pipeline.launch_provider(sip_port, CALL_DURATION, true))
                << "Failed to relaunch provider for call " << (call + 2);
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }

    EXPECT_FALSE(any_crashed) << "Service crashed during multi-call test";
    std::printf("  =====================================================\n\n");
}

TEST_F(EndToEndTest, OneHourStressTest) {
    const int duration_s = []() {
        const char* env = std::getenv("STRESS_TEST_DURATION_SECONDS");
        return env ? std::atoi(env) : 3600;
    }();
    const int call_duration = 60;

    int sip_port = find_free_port();

    std::printf("\n  ========== STRESS TEST (real models, %ds) ==========\n", duration_s);
    std::printf("  SIP port:       %d\n", sip_port);
    std::printf("  Call duration:  %ds each\n", call_duration);

    ASSERT_TRUE(pipeline.launch_provider(sip_port, call_duration, true));
    ASSERT_TRUE(pipeline.launch_services(2, "stress", "127.0.0.1", sip_port));

    std::this_thread::sleep_for(std::chrono::seconds(10));
    ASSERT_TRUE(pipeline.all_alive()) << "Services crashed during startup";

    auto start = std::chrono::steady_clock::now();
    bool any_crashed = false;
    int call_count = 0;

    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(duration_s)) {
        call_count++;
        std::printf("  Call %d started\n", call_count);

        for (int s = 0; s < call_duration + 5; s++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!pipeline.all_alive()) {
                any_crashed = true;
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start).count();
                std::printf("  SERVICE CRASH at t=%lds during call %d\n", (long)elapsed, call_count);
                break;
            }
            if (std::chrono::steady_clock::now() - start >= std::chrono::seconds(duration_s)) break;
        }
        if (any_crashed) break;
        if (std::chrono::steady_clock::now() - start >= std::chrono::seconds(duration_s)) break;

        kill_process(pipeline.provider);
        pipeline.provider = -1;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!pipeline.launch_provider(sip_port, call_duration, true)) {
            std::printf("  Failed to relaunch provider for call %d\n", call_count + 1);
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    auto total = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    std::printf("  Duration:  %lds\n", (long)total);
    std::printf("  Calls:     %d completed\n", call_count);
    std::printf("  Crashed:   %s\n", any_crashed ? "YES" : "NO");
    std::printf("  =====================================================\n\n");

    EXPECT_FALSE(any_crashed) << "Service crashed during stress test";
}

TEST_F(EndToEndTest, MultiLineSimultaneous) {
    int sip_port = find_free_port();
    constexpr int LINES = 6;
    constexpr int DURATION = 25;

    std::printf("\n  ========== MULTI-LINE SIMULTANEOUS (%d lines, %ds) ==========\n", LINES, DURATION);
    std::printf("  SIP provider port: %d\n", sip_port);
    std::printf("  %d simultaneous lines, injected audio\n", LINES);

    ASSERT_TRUE(pipeline.launch_provider(sip_port, DURATION, true));
    ASSERT_TRUE(pipeline.launch_services(LINES, "multiline", "127.0.0.1", sip_port));

    std::this_thread::sleep_for(std::chrono::seconds(12));
    ASSERT_TRUE(pipeline.all_alive()) << "Services crashed during startup";

    std::printf("  All services alive. %d simultaneous calls in progress.\n", LINES);

    bool any_crashed = false;
    for (int i = 0; i < DURATION; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!pipeline.all_alive()) {
            any_crashed = true;
            std::printf("  SERVICE CRASH at t=%ds\n", i + 1);
            break;
        }
        if ((i + 1) % 15 == 0) {
            std::printf("  t=%ds - all services alive with %d concurrent lines\n", i + 1, LINES);
        }
    }

    EXPECT_FALSE(any_crashed) << "Service crashed during multi-line simultaneous test";
    std::printf("  ============================================================\n\n");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    const char* bin = std::getenv("WHISPERTALK_BIN_DIR");
    g_bin_dir = bin ? bin : "";
    if (g_bin_dir.empty()) {
        g_bin_dir = "bin";
    }

    const char* models = std::getenv("WHISPERTALK_MODELS_DIR");
    g_models_dir = models ? models : "";
    if (g_models_dir.empty()) {
        g_models_dir = g_bin_dir + "/models";
    }

    std::printf("Integration test config:\n");
    std::printf("  BIN_DIR:    %s\n", g_bin_dir.c_str());
    std::printf("  MODELS_DIR: %s\n", g_models_dir.c_str());

    return RUN_ALL_TESTS();
}
