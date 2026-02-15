// LLaMA Service (Interconnect-based, Apple Silicon optimized)
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include "interconnect.h"
#include "llama.h"

struct LlamaChatMessage {
    std::string role;
    std::string content;
};

struct LlamaCall {
    uint32_t id;
    int seq_id;
    int n_past = 0;
    std::vector<LlamaChatMessage> messages;
    std::chrono::steady_clock::time_point last_activity;
    std::atomic<bool> generating{false};
};

class LlamaService {
public:
    LlamaService(const std::string& model_path) 
        : running_(true),
          next_seq_id_(0),
          interconnect_(whispertalk::ServiceType::LLAMA_SERVICE) {
        llama_backend_init();
        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = -1;
        model_ = llama_model_load_from_file(model_path.c_str(), mparams);
        if (!model_) {
            throw std::runtime_error("Failed to load model: " + model_path);
        }
        
        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = 2048;
        cparams.n_threads = 8;
        cparams.n_threads_batch = 8;
        ctx_ = llama_init_from_model(model_, cparams);
        if (!ctx_) {
            throw std::runtime_error("Failed to initialize context");
        }
        
        vocab_ = llama_model_get_vocab(model_);
        sampler_ = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(sampler_, llama_sampler_init_greedy());
        
        std::cout << "🚀 LLaMA Service optimized for Apple Silicon (Metal) initialized" << std::endl;
    }

    ~LlamaService() {
        if (sampler_) llama_sampler_free(sampler_);
        if (ctx_) llama_free(ctx_);
        if (model_) llama_model_free(model_);
        llama_backend_free();
    }

    bool init() {
        if (!interconnect_.initialize()) {
            std::cerr << "Failed to initialize interconnect" << std::endl;
            return false;
        }

        std::cout << "🔗 Interconnect initialized (master=" << interconnect_.is_master() << ")" << std::endl;

        if (!interconnect_.connect_to_downstream()) {
            std::cout << "⚠️  Downstream (Kokoro) not available yet - will auto-reconnect" << std::endl;
        }

        interconnect_.register_call_end_handler([this](uint32_t call_id) {
            this->handle_call_end(call_id);
        });

        return true;
    }

    void run() {
        std::thread receiver_thread(&LlamaService::receiver_loop, this);
        
        std::cout << "🇩🇪 LLaMA German Service running" << std::endl;
        
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        receiver_thread.join();
        interconnect_.shutdown();
    }

private:
    void receiver_loop() {
        while (running_) {
            whispertalk::Packet pkt;
            if (!interconnect_.recv_from_upstream(pkt, 100)) {
                continue;
            }

            if (!pkt.is_valid() || pkt.payload_size == 0) {
                continue;
            }

            std::string text(reinterpret_cast<const char*>(pkt.payload.data()), pkt.payload_size);
            
            std::thread([this, call_id = pkt.call_id, text]() {
                std::string response = this->process_call(call_id, text);
                if (!response.empty()) {
                    this->send_to_tts(call_id, response);
                }
            }).detach();
        }
    }

    std::string process_call(uint32_t cid, const std::string& text) {
        auto call = get_or_create_call(cid);
        call->last_activity = std::chrono::steady_clock::now();

        if (call->generating.exchange(true)) {
            std::cout << "⚠️  [" << cid << "] Interrupting previous generation" << std::endl;
            call->generating = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::lock_guard<std::mutex> lock(llama_mutex_);

        call->messages.push_back({"user", text});

        std::vector<llama_chat_message> chat_msgs;
        chat_msgs.push_back({"system", "Du bist ein extrem effizienter Telefon-Assistent. Antworte IMMER auf DEUTSCH. Deine Antworten sind extrem kurz (max. 15 Wörter). Sei höflich aber komm sofort zum Punkt."});
        
        for (const auto& m : call->messages) {
            chat_msgs.push_back({m.role.c_str(), m.content.c_str()});
        }

        const char* tmpl = llama_model_chat_template(model_, nullptr);
        std::vector<char> formatted(4096);
        int32_t len = llama_chat_apply_template(tmpl, chat_msgs.data(), chat_msgs.size(), true, formatted.data(), formatted.size());
        if (len > (int32_t)formatted.size()) {
            formatted.resize(len);
            len = llama_chat_apply_template(tmpl, chat_msgs.data(), chat_msgs.size(), true, formatted.data(), formatted.size());
        }
        
        std::string prompt(formatted.data(), len);
        std::vector<llama_token> tokens = tokenize(prompt, true);

        if (tokens.empty()) {
            std::cerr << "Error: No tokens generated for prompt" << std::endl;
            return "Fehler.";
        }

        llama_memory_t mem = llama_get_memory(ctx_);
        llama_memory_seq_rm(mem, call->seq_id, -1, -1);
        call->n_past = 0;

        llama_batch batch = llama_batch_init(tokens.size(), 0, 1);
        batch.n_tokens = tokens.size();
        for (size_t i = 0; i < tokens.size(); ++i) {
            batch.token[i] = tokens[i];
            batch.pos[i] = i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = call->seq_id;
            batch.logits[i] = (i == tokens.size() - 1);
        }

        if (llama_decode(ctx_, batch) != 0) {
            llama_batch_free(batch);
            return "Fehler.";
        }
        call->n_past = tokens.size();
        llama_batch_free(batch);

        std::string response;
        llama_token id;
        for (int i = 0; i < 64; ++i) {
            if (!call->generating) {
                std::cout << "⚠️  [" << cid << "] Generation interrupted" << std::endl;
                break;
            }

            id = llama_sampler_sample(sampler_, ctx_, -1);
            if (id == llama_vocab_eos(vocab_)) break;
            
            char piece[128];
            int n = llama_token_to_piece(vocab_, id, piece, sizeof(piece), 0, false);
            if (n > 0) response.append(piece, n);

            if (response.find('.') != std::string::npos || 
                response.find('?') != std::string::npos || 
                response.find('!') != std::string::npos) {
                break;
            }

            llama_batch b = llama_batch_init(1, 0, 1);
            b.n_tokens = 1;
            b.token[0] = id;
            b.pos[0] = call->n_past;
            b.n_seq_id[0] = 1;
            b.seq_id[0][0] = call->seq_id;
            b.logits[0] = true;
            
            if (llama_decode(ctx_, b) != 0) {
                llama_batch_free(b);
                break;
            }
            call->n_past++;
            llama_batch_free(b);
        }

        call->generating = false;

        size_t start = response.find_first_not_of(" \n\r\t");
        if (start != std::string::npos) response = response.substr(start);

        call->messages.push_back({"assistant", response});
        std::cout << "🦙 [" << cid << "] DE: " << response << std::endl;
        return response;
    }

    std::vector<llama_token> tokenize(const std::string& text, bool bos) {
        std::vector<llama_token> res(text.size() + 2);
        int n = llama_tokenize(vocab_, text.c_str(), text.size(), res.data(), res.size(), bos, true);
        res.resize(n);
        return res;
    }

    void send_to_tts(uint32_t cid, const std::string& text) {
        whispertalk::Packet pkt(cid, text.c_str(), text.length());
        if (!interconnect_.send_to_downstream(pkt)) {
            if (interconnect_.downstream_state() != whispertalk::ConnectionState::CONNECTED) {
                std::cout << "⚠️  [" << cid << "] Kokoro disconnected, discarding response to /dev/null" << std::endl;
            }
        }
    }

    std::shared_ptr<LlamaCall> get_or_create_call(uint32_t cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(cid)) return calls_[cid];
        auto call = std::make_shared<LlamaCall>();
        call->id = cid;
        call->seq_id = next_seq_id_++;
        call->last_activity = std::chrono::steady_clock::now();
        calls_[cid] = call;
        std::cout << "📞 Created conversation context for call_id " << cid << std::endl;
        return call;
    }

    void handle_call_end(uint32_t call_id) {
        std::lock_guard<std::mutex> calls_lock(calls_mutex_);
        if (calls_.count(call_id)) {
            std::lock_guard<std::mutex> llama_lock(llama_mutex_);
            llama_memory_t mem = llama_get_memory(ctx_);
            llama_memory_seq_rm(mem, calls_[call_id]->seq_id, -1, -1);
            calls_.erase(call_id);
            std::cout << "🛑 Call " << call_id << " ended, clearing conversation context" << std::endl;
        }
    }

    std::atomic<bool> running_;
    int next_seq_id_;
    struct llama_model* model_ = nullptr;
    struct llama_context* ctx_ = nullptr;
    const struct llama_vocab* vocab_ = nullptr;
    struct llama_sampler* sampler_ = nullptr;
    std::mutex llama_mutex_;
    std::mutex calls_mutex_;
    std::map<uint32_t, std::shared_ptr<LlamaCall>> calls_;
    whispertalk::InterconnectNode interconnect_;
};

int main(int argc, char** argv) {
    const char* env_models = std::getenv("WHISPERTALK_MODELS_DIR");
    std::string models_dir = env_models ? env_models :
#ifdef WHISPERTALK_MODELS_DIR
        WHISPERTALK_MODELS_DIR;
#else
        "models";
#endif
    std::string model_path = models_dir + "/Llama-3.2-1B-Instruct-Q8_0.gguf";
    if (argc >= 2) model_path = argv[1];
    
    try {
        LlamaService service(model_path);
        if (!service.init()) {
            return 1;
        }
        service.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
