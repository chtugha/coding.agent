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

static bool dir_has_file_with_suffix(const std::string& dir, const std::string& suffix,
                                     const std::string& prefix = "") {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name(entry->d_name);
        if (name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 &&
            (prefix.empty() || (name.size() >= prefix.size() &&
             name.compare(0, prefix.size(), prefix) == 0))) {
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
    pid_t tts = -1;        // Generic TTS stage/dock (tts-service binary)
    pid_t kokoro = -1;     // Kokoro engine — docks into TTS stage
    pid_t neutts = -1;     // Optional NeuTTS engine — docks into TTS stage
    pid_t oap = -1;
    pid_t provider = -1;

    std::vector<std::pair<std::string,std::string>> model_env() {
        return {{"WHISPERTALK_MODELS_DIR", g_models_dir}};
    }

    bool launch_provider(int sip_port) {
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

        // Generic TTS dock/stage MUST come up before any engine so the
        // engine's HELLO handshake succeeds on its first connection attempt.
        tts = launch_process("tts-service", {}, env);
        if (tts <= 0) return false;
        std::this_thread::sleep_for(std::chrono::seconds(2));

        kokoro = launch_process("kokoro-service", {}, env);
        if (kokoro <= 0) return false;
        std::this_thread::sleep_for(std::chrono::seconds(5));

        oap = launch_process("outbound-audio-processor", {}, env);
        if (oap <= 0) return false;
        std::this_thread::sleep_for(std::chrono::seconds(2));

        return true;
    }

    bool all_alive() {
        // NeuTTS is optional — only checked if it was actually launched.
        bool neutts_ok = (neutts <= 0) || process_alive(neutts);
        return process_alive(sip) && process_alive(iap) && process_alive(vad) &&
               process_alive(whisper) && process_alive(llama) &&
               process_alive(tts) && process_alive(kokoro) && neutts_ok &&
               process_alive(oap);
    }

    void shutdown() {
        kill_process(provider);
        kill_process(oap);
        kill_process(neutts);
        kill_process(kokoro);
        kill_process(tts);
        kill_process(llama);
        kill_process(whisper);
        kill_process(vad);
        kill_process(iap);
        kill_process(sip);
        provider = sip = iap = vad = whisper = llama = tts = kokoro = neutts = oap = -1;
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

    // NeuTTS engine diagnostic cmd port is 13174 (separate from the TTS
    // dock's cmd port at 13142 — neutts no longer hosts the pipeline
    // interconnect itself, it docks into tts-service).
    constexpr uint16_t CMD_PORT = 13174;
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
            "tts-service", "kokoro-service", "outbound-audio-processor"
        };
        for (const char* bin : required) {
            if (!binary_exists(bin))
                GTEST_SKIP() << "Pipeline binary not found: " << g_bin_dir << "/" << bin;
        }

        if (!dir_has_file_with_suffix(g_models_dir, ".gguf"))
            GTEST_SKIP() << "No .gguf LLaMA model found in " << g_models_dir;
        if (!dir_has_file_with_suffix(g_models_dir, ".bin", "ggml-"))
            GTEST_SKIP() << "No whisper model (ggml-*.bin) found in " << g_models_dir;
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

    ASSERT_TRUE(pipeline.launch_provider(sip_port));
    ASSERT_TRUE(pipeline.launch_services(2, "test", "127.0.0.1", sip_port));

    std::printf("\n  ========== SINGLE CALL END-TO-END (real models) ==========\n");
    std::printf("  SIP provider port: %d\n", sip_port);
    std::printf("  2 lines -> 1 call -> 2 call_ids through pipeline\n");
    std::printf("  Waiting for registration + call setup...\n");

    std::this_thread::sleep_for(std::chrono::seconds(10));
    ASSERT_TRUE(pipeline.all_alive()) << "One or more services crashed during startup";

    std::printf("  All 8 services alive (incl. generic TTS dock). Call in progress.\n");
    std::printf("  Monitoring pipeline stability for 30 seconds...\n");

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
    std::printf("  Each call: 2 lines, %ds monitoring window\n", CALL_DURATION);

    ASSERT_TRUE(pipeline.launch_provider(sip_port));
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
            ASSERT_TRUE(pipeline.launch_provider(sip_port))
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

    ASSERT_TRUE(pipeline.launch_provider(sip_port));
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
        if (!pipeline.launch_provider(sip_port)) {
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

// Spec §5.4 scenario: switch engine mid-call.
//
// While a call is live with kokoro docked, launch neutts-service. The TTS
// dock uses "last-connect-wins": the new engine's HELLO triggers a swap,
// kokoro receives SHUTDOWN, dock flips to neutts, SIP leg stays intact.
TEST_F(EndToEndTest, SwitchEngineMidCall) {
    if (!binary_exists("neutts-service"))
        GTEST_SKIP() << "neutts-service binary not found — swap scenario needs a second engine";
    {
        std::string neutts_dir = g_models_dir + "/neutts-nano-german";
        if (!file_exists(neutts_dir + "/ref_codes.bin") ||
            !file_exists(neutts_dir + "/neucodec_decoder.mlmodelc"))
            GTEST_SKIP() << "NeuTTS model files not found in " << neutts_dir;
    }

    int sip_port = find_free_port();
    ASSERT_TRUE(pipeline.launch_provider(sip_port));
    ASSERT_TRUE(pipeline.launch_services(2, "swap", "127.0.0.1", sip_port));

    std::this_thread::sleep_for(std::chrono::seconds(10));
    ASSERT_TRUE(pipeline.all_alive()) << "Services crashed during startup";

    // Query the TTS dock: active engine must be "kokoro".
    constexpr uint16_t TTS_DOCK_CMD_PORT = 13142;
    std::string before = send_cmd(TTS_DOCK_CMD_PORT, "STATUS", 5);
    std::printf("  Dock STATUS before swap: '%s'\n", before.c_str());
    ASSERT_NE(before.find("ACTIVE kokoro"), std::string::npos)
        << "TTS dock did not report kokoro as active engine before swap";

    // Launch the second engine. The dock must swap within a few seconds.
    pipeline.neutts = launch_process("neutts-service", {}, pipeline.model_env());
    ASSERT_GT(pipeline.neutts, 0);

    bool swapped = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::string st = send_cmd(TTS_DOCK_CMD_PORT, "STATUS", 5);
        if (st.find("ACTIVE neutts") != std::string::npos) {
            swapped = true;
            break;
        }
    }
    EXPECT_TRUE(swapped) << "TTS dock did not swap to neutts within 120s";

    // SIP call must still be in progress — provider + sip-client alive.
    EXPECT_TRUE(process_alive(pipeline.sip)) << "sip-client died during engine swap";
    EXPECT_TRUE(process_alive(pipeline.provider)) << "provider died during engine swap";
    EXPECT_TRUE(process_alive(pipeline.tts)) << "tts-service died during engine swap";
    EXPECT_TRUE(process_alive(pipeline.llama)) << "llama-service died during engine swap";
    EXPECT_TRUE(process_alive(pipeline.oap)) << "oap died during engine swap";
}

// Spec §5.4 scenario: SIGKILL the active engine, restart it, audio resumes.
TEST_F(EndToEndTest, KillAndRestartEngine) {
    int sip_port = find_free_port();
    ASSERT_TRUE(pipeline.launch_provider(sip_port));
    ASSERT_TRUE(pipeline.launch_services(2, "restart", "127.0.0.1", sip_port));

    std::this_thread::sleep_for(std::chrono::seconds(10));
    ASSERT_TRUE(pipeline.all_alive()) << "Services crashed during startup";

    constexpr uint16_t TTS_DOCK_CMD_PORT = 13142;
    std::string before = send_cmd(TTS_DOCK_CMD_PORT, "STATUS", 5);
    ASSERT_NE(before.find("ACTIVE kokoro"), std::string::npos)
        << "Expected kokoro to be active before kill (got: " << before << ")";

    // SIGKILL the kokoro engine (not SIGTERM — simulate a crash).
    ASSERT_GT(pipeline.kokoro, 0);
    kill(pipeline.kokoro, SIGKILL);
    int status = 0;
    waitpid(pipeline.kokoro, &status, 0);
    pipeline.kokoro = -1;

    // Dock must report NONE within 15s.
    bool cleared = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string st = send_cmd(TTS_DOCK_CMD_PORT, "STATUS", 5);
        if (st.find("NONE") != std::string::npos) {
            cleared = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    EXPECT_TRUE(cleared) << "TTS dock did not clear slot after engine SIGKILL";

    // Other pipeline services must still be alive.
    EXPECT_TRUE(process_alive(pipeline.tts)) << "tts-service died after engine kill";
    EXPECT_TRUE(process_alive(pipeline.llama)) << "llama-service died after engine kill";
    EXPECT_TRUE(process_alive(pipeline.oap)) << "oap died after engine kill";
    EXPECT_TRUE(process_alive(pipeline.sip)) << "sip-client died after engine kill";

    // Restart kokoro; dock must re-dock it.
    pipeline.kokoro = launch_process("kokoro-service", {}, pipeline.model_env());
    ASSERT_GT(pipeline.kokoro, 0);

    bool redocked = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::string st = send_cmd(TTS_DOCK_CMD_PORT, "STATUS", 5);
        if (st.find("ACTIVE kokoro") != std::string::npos) {
            redocked = true;
            break;
        }
    }
    EXPECT_TRUE(redocked) << "TTS dock did not re-dock kokoro within 120s";
}

TEST_F(EndToEndTest, MultiLineSimultaneous) {
    int sip_port = find_free_port();
    constexpr int LINES = 6;
    constexpr int DURATION = 25;

    std::printf("\n  ========== MULTI-LINE SIMULTANEOUS (%d lines, %ds) ==========\n", LINES, DURATION);
    std::printf("  SIP provider port: %d\n", sip_port);
    std::printf("  %d simultaneous lines\n", LINES);

    ASSERT_TRUE(pipeline.launch_provider(sip_port));
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
