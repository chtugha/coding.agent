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

static std::string g_bin_dir;
static std::string g_models_dir;

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
    pid_t whisper = -1;
    pid_t llama = -1;
    pid_t kokoro = -1;
    pid_t oap = -1;
    pid_t provider = -1;

    std::vector<std::pair<std::string,std::string>> model_env() {
        return {{"WHISPERTALK_MODELS_DIR", g_models_dir}};
    }

    bool launch_provider(int sip_port, int duration_s, bool inject = true) {
        std::vector<std::string> pargs = {
            "--port", std::to_string(sip_port),
            "--duration", std::to_string(duration_s)
        };
        if (inject) pargs.push_back("--inject");
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
        return process_alive(sip) && process_alive(iap) && process_alive(whisper) &&
               process_alive(llama) && process_alive(kokoro) && process_alive(oap);
    }

    void shutdown() {
        kill_process(provider);
        kill_process(oap);
        kill_process(kokoro);
        kill_process(llama);
        kill_process(whisper);
        kill_process(iap);
        kill_process(sip);
        provider = sip = iap = whisper = llama = kokoro = oap = -1;
    }
};

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

    std::printf("  All 6 services alive. Call in progress.\n");
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
