#include <iostream>
#include <vector>
#include <array>
#include <cstring>
#include <random>
#include <chrono>
#include <thread>
#include <algorithm>
using namespace std;

// Simple simulation of Kokoro->Outbound->SHM->SIP pipeline timing for 10 utterances
// Models: leading silence trimming, fast-start of first non-silence frame, burst drain to SHM

struct SimOutChannel {
    vector<array<uint8_t,160>> shm;
    bool write_frame(const uint8_t* p, size_t n) {
        if (n != 160) return false; // frame size
        array<uint8_t,160> a; memcpy(a.data(), p, 160); shm.emplace_back(a); return true;
    }
};

static inline bool is_all_mulaw_silence(const uint8_t* p) {
    for (int i=0;i<160;++i) if (p[i] != 0xFF) return false; return true;
}

struct OutboundSim {
    // state
    vector<uint8_t> out_buffer; // g711 µ-law queued
    bool pending_first_rtp = false;
    SimOutChannel* out_ch = nullptr;

    // enqueue with bounded buffer (like kMaxBytes=160*100)
    void enqueue_g711(const vector<uint8_t>& g711) {
        const size_t kMaxBytes = 160*100; // 2s
        // Allow long wait before dropping if pre-RTP to avoid losing the beginning
        int spins = 0;
        for (;;) {
            if (out_buffer.size() + g711.size() <= kMaxBytes) {
                out_buffer.insert(out_buffer.end(), g711.begin(), g711.end());
                return;
            }
            if (++spins <= 500) { // wait up to ~1s
                this_thread::sleep_for(chrono::milliseconds(2));
                continue;
            }
            // After 1s, drop oldest frame(s)
            size_t needed = (out_buffer.size() + g711.size()) - kMaxBytes;
            size_t drop = ((needed + 159)/160)*160;
            drop = min(drop, out_buffer.size());
            if (drop) out_buffer.erase(out_buffer.begin(), out_buffer.begin()+drop);
        }
    }

    // handle Kokoro bytes (already µ-law 8kHz frames sequence)
    void handle_bytes(vector<uint8_t> bytes, int64_t now_ms, int run_id) {
        if (bytes.empty()) return;
        // set pending start if buffer is empty
        if (!pending_first_rtp && out_buffer.size() < 160) {
            pending_first_rtp = true;
            cout << "⏱️  [sim] Outbound received first chunk ts=" << now_ms << " run=" << run_id << "\n";
        }
        if (out_ch && bytes.size() >= 160 && pending_first_rtp) {
            size_t off=0;
            while (bytes.size()-off >= 160 && is_all_mulaw_silence(bytes.data()+off)) off+=160;
            if (bytes.size()-off >= 160) {
                bool wrote = out_ch->write_frame(bytes.data()+off, 160);
                if (wrote) {
                    pending_first_rtp = false;
                    cout << "t3: [sim] First RTP frame sent ts=" << now_ms << " run=" << run_id << "\n";
                    size_t rem = off+160;
                    if (bytes.size() > rem) {
                        vector<uint8_t> rest(bytes.begin()+rem, bytes.end());
                        enqueue_g711(rest);
                    }
                    return;
                }
            }
            // only silence so far → enqueue trimmed remainder
            if (off > 0 && bytes.size() > off) {
                vector<uint8_t> rest(bytes.begin()+off, bytes.end());
                enqueue_g711(rest);
                return;
            }
        }
        enqueue_g711(bytes);
    }

    // drain up to kBurstFrames per 20ms tick into SHM
    void scheduler_tick(int kBurstFrames=16) {
        for (int n=0;n<kBurstFrames;++n) {
            if (out_buffer.size() < 160) break;
            (void)out_ch->write_frame(out_buffer.data(), 160);
            out_buffer.erase(out_buffer.begin(), out_buffer.begin()+160);
        }
    }
};

// Generate simulated Kokoro utterance: L leading silence frames, then A audio frames
static vector<uint8_t> gen_utterance_bytes(int L_silence_frames, int A_audio_frames) {
    vector<uint8_t> v; v.reserve(160*(L_silence_frames+A_audio_frames));
    for (int i=0;i<L_silence_frames;++i) v.insert(v.end(), 160, 0xFF);
    for (int j=0;j<A_audio_frames;++j) {
        for (int k=0;k<160;++k) {
            uint8_t b = (k%2==0)? 0x7F : 0x00; // not 0xFF -> treated as audio
            v.push_back(b);
        }
    }
    return v;
}

int main() {
    OutboundSim sim; SimOutChannel ch; sim.out_ch = &ch;
    std::mt19937 rng(123);

    cout << "=== TTS→Outbound→SIP simulation (10 runs) ===\n";
    for (int run=1; run<=10; ++run) {
        sim.out_buffer.clear(); sim.pending_first_rtp=false; ch.shm.clear();
        // t1 at 0ms; Kokoro first audio at ~300-400ms
        int64_t t1 = 0; int64_t t2 = 300 + (rng()%120);
        int L = 0 + (rng()%4);        // 0-3 silence frames (0-60ms)
        int A = 120 + (rng()%80);     // 2.4s-4.0s audio burst
        auto bytes = gen_utterance_bytes(L, A);

        // Simulate chunking: send in 5 subchunks
        int chunk = bytes.size()/5;
        int64_t now = t2; size_t off=0;
        for (int i=0;i<5;++i) {
            size_t take = (i==4)? (bytes.size()-off) : (size_t)chunk;
            vector<uint8_t> part(bytes.begin()+off, bytes.begin()+off+take);
            sim.handle_bytes(move(part), now, run);
            // assume Kokoro quickly sends next at +5ms
            now += 5;
            off += take;
        }
        // Drive scheduler until SHM has at least first audible and a few more frames
        int ticks=0; while (ch.shm.size() < (size_t)min(10, A)) { sim.scheduler_tick(16); this_thread::sleep_for(chrono::milliseconds(1)); if (++ticks > 2000) break; }
        cout << "run="<<run<<" t1→t2=~"<<(t2-t1)<<"ms, first SHM frames="<<ch.shm.size()<<" (expect >=1)\n";
    }
    cout << "=== done ===\n";
    return 0;
}

