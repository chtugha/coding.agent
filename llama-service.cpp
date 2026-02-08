// LLaMA Service (Consolidated)
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include "llama.h"

struct LlamaCall {
    int id;
    int seq_id;
    int n_past = 0;
    std::string history;
};

class LlamaService {
public:
    LlamaService(const std::string& model_path) : running_(true) {
        llama_backend_init();
        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = 99; // Use GPU
        model_ = llama_model_load_from_file(model_path.c_str(), mparams);
        
        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = 4096;
        cparams.n_threads = 4;
        ctx_ = llama_init_from_model(model_, cparams);
        
        vocab_ = llama_model_get_vocab(model_);
        sampler_ = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(sampler_, llama_sampler_init_greedy());
    }

    ~LlamaService() {
        llama_sampler_free(sampler_);
        llama_free(ctx_);
        llama_model_free(model_);
        llama_backend_free();
    }

    void run() {
        int lsock = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1; setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(8083);
        addr.sin_addr.s_addr = INADDR_ANY;
        bind(lsock, (struct sockaddr*)&addr, sizeof(addr));
        listen(lsock, 5);

        std::cout << "🦙 LLaMA Service listening on port 8083" << std::endl;

        while (running_) {
            int csock = accept(lsock, NULL, NULL);
            if (csock < 0) continue;
            char buf[4096];
            ssize_t n = recv(csock, buf, sizeof(buf)-1, 0);
            if (n > 0) {
                buf[n] = '\0';
                std::string msg(buf);
                size_t sep = msg.find(':');
                if (sep != std::string::npos) {
                    int cid = std::stoi(msg.substr(0, sep));
                    std::string text = msg.substr(sep + 1);
                    std::string response = process_call(cid, text);
                    send_to_tts(cid, response);
                }
            }
            close(csock);
        }
    }

private:
    std::string process_call(int cid, const std::string& text) {
        auto call = get_or_create_call(cid);
        std::lock_guard<std::mutex> lock(llama_mutex_);

        std::string prompt = "User: " + text + "\nAssistant:";
        std::vector<llama_token> tokens = tokenize(prompt, false);

        llama_batch batch = llama_batch_init(tokens.size(), 0, 1);
        for (size_t i = 0; i < tokens.size(); ++i) {
            batch.token[i] = tokens[i];
            batch.pos[i] = call->n_past + i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = call->seq_id;
            batch.logits[i] = (i == tokens.size() - 1);
        }

        if (llama_decode(ctx_, batch) != 0) return "";
        call->n_past += tokens.size();
        llama_batch_free(batch);

        std::string response;
        for (int i = 0; i < 64; ++i) {
            llama_token id = llama_sampler_sample(sampler_, ctx_, -1);
            if (id == llama_vocab_eos(vocab_)) break;
            
            char piece[128];
            int n = llama_token_to_piece(vocab_, id, piece, sizeof(piece), 0, false);
            if (n > 0) response.append(piece, n);

            llama_batch b = llama_batch_get_one(&id, 1);
            b.pos[0] = call->n_past;
            b.seq_id[0][0] = call->seq_id;
            if (llama_decode(ctx_, b) != 0) break;
            call->n_past++;
        }

        std::cout << "🦙 [" << cid << "] Response: " << response << std::endl;
        return response;
    }

    std::vector<llama_token> tokenize(const std::string& text, bool bos) {
        std::vector<llama_token> res(text.size() + (bos ? 1 : 0));
        int n = llama_tokenize(vocab_, text.c_str(), text.size(), res.data(), res.size(), bos, true);
        res.resize(n);
        return res;
    }

    void send_to_tts(int cid, const std::string& text) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(8090); // Kokoro port
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            std::string msg = std::to_string(cid) + ":" + text;
            send(sock, msg.c_str(), msg.length(), 0);
        }
        close(sock);
    }

    std::shared_ptr<LlamaCall> get_or_create_call(int cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(cid)) return calls_[cid];
        auto call = std::make_shared<LlamaCall>();
        call->id = cid;
        call->seq_id = calls_.size();
        calls_[cid] = call;
        return call;
    }

    std::atomic<bool> running_;
    struct llama_model* model_ = nullptr;
    struct llama_context* ctx_ = nullptr;
    const struct llama_vocab* vocab_ = nullptr;
    struct llama_sampler* sampler_ = nullptr;
    std::mutex llama_mutex_;
    std::mutex calls_mutex_;
    std::map<int, std::shared_ptr<LlamaCall>> calls_;
};

int main(int argc, char** argv) {
    if (argc < 2) { std::cout << "Usage: llama-service <model_path>" << std::endl; return 1; }
    LlamaService service(argv[1]);
    service.run();
    return 0;
}
