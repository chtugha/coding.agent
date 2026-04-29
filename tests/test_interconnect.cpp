#include <gtest/gtest.h>
#include "interconnect.h"
#include "tts-engine-client.h"
#include <thread>
#include <chrono>
#include <vector>
#include <set>
#include <atomic>
#include <string>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

using namespace whispertalk;

TEST(PacketTest, SerializationRoundTrip) {
    const char* test_data = "Hello, World!";
    Packet original(42, test_data, strlen(test_data));
    
    EXPECT_EQ(original.call_id, 42u);
    EXPECT_EQ(original.payload_size, strlen(test_data));
    EXPECT_TRUE(original.is_valid());
    
    auto serialized = original.serialize();
    EXPECT_EQ(serialized.size(), 8 + strlen(test_data));
    
    Packet deserialized;
    EXPECT_TRUE(Packet::deserialize(serialized.data(), serialized.size(), deserialized));
    
    EXPECT_EQ(deserialized.call_id, original.call_id);
    EXPECT_EQ(deserialized.payload_size, original.payload_size);
    EXPECT_EQ(deserialized.payload, original.payload);
    EXPECT_TRUE(deserialized.is_valid());
}

TEST(PacketTest, InvalidPacketRejection) {
    Packet invalid_call_id(0, "test", 4);
    EXPECT_FALSE(invalid_call_id.is_valid());
    
    Packet too_large(1, nullptr, Packet::MAX_PAYLOAD_SIZE + 1);
    too_large.payload.resize(Packet::MAX_PAYLOAD_SIZE + 1);
    EXPECT_FALSE(too_large.is_valid());
    
    uint8_t short_buffer[4] = {1, 2, 3, 4};
    Packet result;
    EXPECT_FALSE(Packet::deserialize(short_buffer, 4, result));
}

TEST(PacketTest, LargePayload) {
    std::vector<uint8_t> large_data(100000, 0xAB);
    Packet packet(123, large_data.data(), large_data.size());
    
    EXPECT_TRUE(packet.is_valid());
    
    auto serialized = packet.serialize();
    Packet deserialized;
    EXPECT_TRUE(Packet::deserialize(serialized.data(), serialized.size(), deserialized));
    
    EXPECT_EQ(deserialized.call_id, 123u);
    EXPECT_EQ(deserialized.payload_size, large_data.size());
    EXPECT_EQ(deserialized.payload, large_data);
}

TEST(PortAssignmentTest, FixedPortsPerService) {
    EXPECT_EQ(service_base_port(ServiceType::SIP_CLIENT), 13100);
    EXPECT_EQ(service_base_port(ServiceType::INBOUND_AUDIO_PROCESSOR), 13110);
    EXPECT_EQ(service_base_port(ServiceType::VAD_SERVICE), 13115);
    EXPECT_EQ(service_base_port(ServiceType::WHISPER_SERVICE), 13120);
    EXPECT_EQ(service_base_port(ServiceType::LLAMA_SERVICE), 13130);
    EXPECT_EQ(service_base_port(ServiceType::TTS_SERVICE), 13140);
    EXPECT_EQ(service_base_port(ServiceType::OUTBOUND_AUDIO_PROCESSOR), 13150);
    EXPECT_EQ(service_base_port(ServiceType::FRONTEND), 13160);

    // Engine-dock port: TTS_SERVICE exposes +3 for engine docking; others return 0.
    EXPECT_EQ(service_engine_port(ServiceType::TTS_SERVICE), 13143);
    EXPECT_EQ(service_engine_port(ServiceType::LLAMA_SERVICE), 0);
    EXPECT_EQ(service_engine_port(ServiceType::SIP_CLIENT), 0);
}

TEST(PortAssignmentTest, MgmtAndDataPortsAreDeterministic) {
    EXPECT_EQ(service_mgmt_port(ServiceType::SIP_CLIENT), 13100);
    EXPECT_EQ(service_data_port(ServiceType::SIP_CLIENT), 13101);
    EXPECT_EQ(service_cmd_port(ServiceType::SIP_CLIENT), 13102);

    EXPECT_EQ(service_mgmt_port(ServiceType::INBOUND_AUDIO_PROCESSOR), 13110);
    EXPECT_EQ(service_data_port(ServiceType::INBOUND_AUDIO_PROCESSOR), 13111);
}

TEST(PortAssignmentTest, NoPortConflictsAcrossServices) {
    std::set<uint16_t> all_ports;
    ServiceType types[] = {
        ServiceType::SIP_CLIENT,
        ServiceType::INBOUND_AUDIO_PROCESSOR,
        ServiceType::VAD_SERVICE,
        ServiceType::WHISPER_SERVICE,
        ServiceType::LLAMA_SERVICE,
        ServiceType::TTS_SERVICE,
        ServiceType::OUTBOUND_AUDIO_PROCESSOR,
        ServiceType::MOSHI_SERVICE,
        ServiceType::FRONTEND
    };
    for (auto t : types) {
        uint16_t mgmt = service_mgmt_port(t);
        uint16_t data = service_data_port(t);
        uint16_t cmd  = service_cmd_port(t);
        EXPECT_EQ(all_ports.count(mgmt), 0u) << "mgmt port conflict for " << service_type_to_string(t);
        EXPECT_EQ(all_ports.count(data), 0u) << "data port conflict for " << service_type_to_string(t);
        EXPECT_EQ(all_ports.count(cmd), 0u)  << "cmd port conflict for " << service_type_to_string(t);
        all_ports.insert(mgmt);
        all_ports.insert(data);
        all_ports.insert(cmd);
        uint16_t eng = service_engine_port(t);
        if (eng != 0) {
            EXPECT_EQ(all_ports.count(eng), 0u) << "engine port conflict for " << service_type_to_string(t);
            all_ports.insert(eng);
        }
    }
    EXPECT_EQ(all_ports.size(), 28u);
}

TEST(TopologyTest, UpstreamDownstreamMapping) {
    EXPECT_EQ(downstream_of(ServiceType::SIP_CLIENT), ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_EQ(downstream_of(ServiceType::INBOUND_AUDIO_PROCESSOR), ServiceType::VAD_SERVICE);
    EXPECT_EQ(downstream_of(ServiceType::VAD_SERVICE), ServiceType::WHISPER_SERVICE);
    EXPECT_EQ(downstream_of(ServiceType::WHISPER_SERVICE), ServiceType::LLAMA_SERVICE);
    EXPECT_EQ(downstream_of(ServiceType::LLAMA_SERVICE), ServiceType::TTS_SERVICE);
    EXPECT_EQ(downstream_of(ServiceType::TTS_SERVICE), ServiceType::OUTBOUND_AUDIO_PROCESSOR);
    EXPECT_EQ(downstream_of(ServiceType::OUTBOUND_AUDIO_PROCESSOR), ServiceType::SIP_CLIENT);

    EXPECT_EQ(upstream_of(ServiceType::INBOUND_AUDIO_PROCESSOR), ServiceType::SIP_CLIENT);
    EXPECT_EQ(upstream_of(ServiceType::VAD_SERVICE), ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_EQ(upstream_of(ServiceType::WHISPER_SERVICE), ServiceType::VAD_SERVICE);
    EXPECT_EQ(upstream_of(ServiceType::TTS_SERVICE), ServiceType::LLAMA_SERVICE);
    EXPECT_EQ(upstream_of(ServiceType::OUTBOUND_AUDIO_PROCESSOR), ServiceType::TTS_SERVICE);
    EXPECT_EQ(upstream_of(ServiceType::SIP_CLIENT), ServiceType::OUTBOUND_AUDIO_PROCESSOR);
}

TEST(ServiceTypeTest, EnumToString) {
    EXPECT_STREQ(service_type_to_string(ServiceType::SIP_CLIENT), "SIP_CLIENT");
    EXPECT_STREQ(service_type_to_string(ServiceType::INBOUND_AUDIO_PROCESSOR), "INBOUND_AUDIO_PROCESSOR");
    EXPECT_STREQ(service_type_to_string(ServiceType::WHISPER_SERVICE), "WHISPER_SERVICE");
    EXPECT_STREQ(service_type_to_string(ServiceType::LLAMA_SERVICE), "LLAMA_SERVICE");
    EXPECT_STREQ(service_type_to_string(ServiceType::TTS_SERVICE), "TTS_SERVICE");
    EXPECT_STREQ(service_type_to_string(ServiceType::OUTBOUND_AUDIO_PROCESSOR), "OUTBOUND_AUDIO_PROCESSOR");
    EXPECT_STREQ(service_type_to_string(ServiceType::FRONTEND), "FRONTEND");
}

TEST(ConnectionStateTest, InitialStateDisconnected) {
    InterconnectNode node(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(node.initialize());

    EXPECT_EQ(node.upstream_state(), ConnectionState::DISCONNECTED);
    auto ds = node.downstream_state();
    EXPECT_TRUE(ds == ConnectionState::DISCONNECTED || ds == ConnectionState::CONNECTING);

    node.shutdown();
}

TEST(PeerConnectionTest, TwoServicesEstablishConnection) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(upstream.connect_to_downstream());

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_EQ(upstream.downstream_state(), ConnectionState::CONNECTED);
    EXPECT_EQ(downstream.upstream_state(), ConnectionState::CONNECTED);

    downstream.shutdown();
    upstream.shutdown();
}

TEST(PeerConnectionTest, SendAndReceivePackets) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(upstream.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const char* test_data = "Hello from upstream";
    Packet send_pkt(1, test_data, strlen(test_data));
    EXPECT_TRUE(upstream.send_to_downstream(send_pkt));

    Packet recv_pkt;
    EXPECT_TRUE(downstream.recv_from_upstream(recv_pkt, 2000));
    EXPECT_EQ(recv_pkt.call_id, 1u);
    EXPECT_EQ(recv_pkt.payload_size, strlen(test_data));
    EXPECT_EQ(std::string(recv_pkt.payload.begin(), recv_pkt.payload.end()), std::string(test_data));

    const char* reply = "Ack from downstream";
    Packet reply_pkt(1, reply, strlen(reply));
    EXPECT_TRUE(downstream.send_to_upstream(reply_pkt));

    Packet recv_reply;
    EXPECT_TRUE(upstream.recv_from_downstream(recv_reply, 2000));
    EXPECT_EQ(recv_reply.call_id, 1u);
    EXPECT_EQ(std::string(recv_reply.payload.begin(), recv_reply.payload.end()), std::string(reply));

    downstream.shutdown();
    upstream.shutdown();
}

TEST(PeerConnectionTest, Send1000PacketsFullDelivery) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(upstream.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const int NUM_PACKETS = 1000;
    std::atomic<int> received_count(0);
    std::atomic<bool> all_correct(true);

    std::thread receiver([&]() {
        for (int i = 0; i < NUM_PACKETS; ++i) {
            Packet pkt;
            if (downstream.recv_from_upstream(pkt, 5000)) {
                uint32_t expected_id = i + 1;
                if (pkt.call_id != expected_id) {
                    all_correct = false;
                }
                received_count++;
            } else {
                all_correct = false;
                break;
            }
        }
    });

    for (int i = 0; i < NUM_PACKETS; ++i) {
        std::string data = "packet_" + std::to_string(i);
        Packet pkt(i + 1, data.c_str(), data.size());
        EXPECT_TRUE(upstream.send_to_downstream(pkt));
    }

    receiver.join();

    EXPECT_EQ(received_count.load(), NUM_PACKETS);
    EXPECT_TRUE(all_correct.load());

    downstream.shutdown();
    upstream.shutdown();
}

TEST(PeerConnectionTest, BidirectionalSimultaneous) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(upstream.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const int COUNT = 100;
    std::atomic<int> up_recv_count(0);
    std::atomic<int> down_recv_count(0);

    std::thread sender_up([&]() {
        for (int i = 0; i < COUNT; ++i) {
            std::string d = "up" + std::to_string(i);
            Packet p(i + 1, d.c_str(), d.size());
            upstream.send_to_downstream(p);
        }
    });

    std::thread sender_down([&]() {
        for (int i = 0; i < COUNT; ++i) {
            std::string d = "down" + std::to_string(i);
            Packet p(i + 1, d.c_str(), d.size());
            downstream.send_to_upstream(p);
        }
    });

    std::thread receiver_down([&]() {
        for (int i = 0; i < COUNT; ++i) {
            Packet p;
            if (downstream.recv_from_upstream(p, 5000)) {
                down_recv_count++;
            }
        }
    });

    std::thread receiver_up([&]() {
        for (int i = 0; i < COUNT; ++i) {
            Packet p;
            if (upstream.recv_from_downstream(p, 5000)) {
                up_recv_count++;
            }
        }
    });

    sender_up.join();
    sender_down.join();
    receiver_down.join();
    receiver_up.join();

    EXPECT_EQ(down_recv_count.load(), COUNT);
    EXPECT_EQ(up_recv_count.load(), COUNT);

    downstream.shutdown();
    upstream.shutdown();
}

TEST(CallIDReservationTest, AtomicReservationNoCollisions) {
    InterconnectNode node(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(node.initialize());
    
    const int NUM_THREADS = 10;
    std::vector<std::thread> threads;
    std::vector<uint32_t> reserved_ids(NUM_THREADS);
    std::atomic<int> ready_count(0);
    
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&node, &reserved_ids, &ready_count, i]() {
            ready_count++;
            while (ready_count < NUM_THREADS) {
                std::this_thread::yield();
            }
            reserved_ids[i] = node.reserve_call_id(100 + i);
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    std::set<uint32_t> unique_ids(reserved_ids.begin(), reserved_ids.end());
    EXPECT_EQ(unique_ids.size(), static_cast<size_t>(NUM_THREADS));
    
    for (auto id : reserved_ids) {
        EXPECT_GT(id, 0u);
    }
    
    node.shutdown();
}

TEST(MgmtMessageTest, CallEndPropagatesDownstream) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(upstream.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::atomic<bool> received(false);
    std::atomic<uint32_t> received_cid(0);

    downstream.register_call_end_handler([&](uint32_t cid) {
        received_cid = cid;
        received = true;
    });

    uint32_t test_cid = upstream.reserve_call_id(42);
    upstream.broadcast_call_end(test_cid);

    for (int i = 0; i < 50 && !received; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_TRUE(received.load());
    EXPECT_EQ(received_cid.load(), test_cid);

    downstream.shutdown();
    upstream.shutdown();
}

TEST(MgmtMessageTest, DuplicateCallEndIdempotent) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(upstream.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::atomic<int> handler_count(0);
    downstream.register_call_end_handler([&](uint32_t) {
        handler_count++;
    });

    uint32_t cid = upstream.reserve_call_id(1);
    upstream.broadcast_call_end(cid);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    upstream.broadcast_call_end(cid);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    EXPECT_EQ(handler_count.load(), 1);

    downstream.shutdown();
    upstream.shutdown();
}

TEST(MgmtMessageTest, SpeechSignalPropagation) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(upstream.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::atomic<bool> speech_received(false);
    std::atomic<bool> speech_active_val(false);

    downstream.register_speech_signal_handler([&](uint32_t cid, bool active) {
        (void)cid;
        speech_active_val = active;
        speech_received = true;
    });

    upstream.broadcast_speech_signal(1, true);

    for (int i = 0; i < 50 && !speech_received; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_TRUE(speech_received.load());
    EXPECT_TRUE(speech_active_val.load());
    EXPECT_TRUE(upstream.is_speech_active(1));

    downstream.shutdown();
    upstream.shutdown();
}

TEST(ReconnectionTest, AutoReconnectsAfterDownstreamRestart) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    {
        InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
        EXPECT_TRUE(downstream.initialize());

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT_TRUE(upstream.connect_to_downstream());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        EXPECT_EQ(upstream.downstream_state(), ConnectionState::CONNECTED);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    InterconnectNode downstream2(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(downstream2.initialize());

    for (int i = 0; i < 40; ++i) {
        if (upstream.downstream_state() == ConnectionState::CONNECTED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    EXPECT_EQ(upstream.downstream_state(), ConnectionState::CONNECTED);

    const char* data = "reconnected";
    Packet pkt(1, data, strlen(data));
    EXPECT_TRUE(upstream.send_to_downstream(pkt));

    Packet recv_pkt;
    EXPECT_TRUE(downstream2.recv_from_upstream(recv_pkt, 2000));
    EXPECT_EQ(recv_pkt.call_id, 1u);

    downstream2.shutdown();
    upstream.shutdown();
}

TEST(ThreeNodePipelineTest, DataFlowsThroughPipeline) {
    InterconnectNode sip(ServiceType::SIP_CLIENT);
    InterconnectNode iap(ServiceType::INBOUND_AUDIO_PROCESSOR);
    InterconnectNode vad(ServiceType::VAD_SERVICE);
    InterconnectNode whisper(ServiceType::WHISPER_SERVICE);

    EXPECT_TRUE(sip.initialize());
    EXPECT_TRUE(iap.initialize());
    EXPECT_TRUE(vad.initialize());
    EXPECT_TRUE(whisper.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(sip.connect_to_downstream());
    EXPECT_TRUE(iap.connect_to_downstream());
    EXPECT_TRUE(vad.connect_to_downstream());

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_EQ(sip.downstream_state(), ConnectionState::CONNECTED);
    EXPECT_EQ(iap.downstream_state(), ConnectionState::CONNECTED);
    EXPECT_EQ(vad.downstream_state(), ConnectionState::CONNECTED);

    const char* audio = "audio_data_from_sip";
    Packet audio_pkt(1, audio, strlen(audio));
    EXPECT_TRUE(sip.send_to_downstream(audio_pkt));

    Packet iap_recv;
    EXPECT_TRUE(iap.recv_from_upstream(iap_recv, 2000));
    EXPECT_EQ(iap_recv.call_id, 1u);

    const char* processed = "processed_audio";
    Packet processed_pkt(1, processed, strlen(processed));
    EXPECT_TRUE(iap.send_to_downstream(processed_pkt));

    Packet vad_recv;
    EXPECT_TRUE(vad.recv_from_upstream(vad_recv, 2000));
    EXPECT_EQ(vad_recv.call_id, 1u);

    const char* speech_chunk = "speech_chunk";
    Packet speech_pkt(1, speech_chunk, strlen(speech_chunk));
    EXPECT_TRUE(vad.send_to_downstream(speech_pkt));

    Packet whisper_recv;
    EXPECT_TRUE(whisper.recv_from_upstream(whisper_recv, 2000));
    EXPECT_EQ(whisper_recv.call_id, 1u);
    EXPECT_EQ(std::string(whisper_recv.payload.begin(), whisper_recv.payload.end()),
              std::string(speech_chunk));

    whisper.shutdown();
    vad.shutdown();
    iap.shutdown();
    sip.shutdown();
}

TEST(ThreeNodePipelineTest, CallEndPropagatesAcrossMultipleHops) {
    InterconnectNode sip(ServiceType::SIP_CLIENT);
    InterconnectNode iap(ServiceType::INBOUND_AUDIO_PROCESSOR);
    InterconnectNode vad(ServiceType::VAD_SERVICE);
    InterconnectNode whisper(ServiceType::WHISPER_SERVICE);

    EXPECT_TRUE(sip.initialize());
    EXPECT_TRUE(iap.initialize());
    EXPECT_TRUE(vad.initialize());
    EXPECT_TRUE(whisper.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(sip.connect_to_downstream());
    EXPECT_TRUE(iap.connect_to_downstream());
    EXPECT_TRUE(vad.connect_to_downstream());

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::atomic<bool> iap_received(false);
    std::atomic<bool> vad_received(false);
    std::atomic<bool> whisper_received(false);

    iap.register_call_end_handler([&](uint32_t cid) {
        (void)cid;
        iap_received = true;
    });

    vad.register_call_end_handler([&](uint32_t cid) {
        (void)cid;
        vad_received = true;
    });

    whisper.register_call_end_handler([&](uint32_t cid) {
        (void)cid;
        whisper_received = true;
    });

    uint32_t cid = sip.reserve_call_id(10);
    sip.broadcast_call_end(cid);

    for (int i = 0; i < 50; ++i) {
        if (iap_received && vad_received && whisper_received) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(iap_received.load());
    EXPECT_TRUE(vad_received.load());
    EXPECT_TRUE(whisper_received.load());

    whisper.shutdown();
    vad.shutdown();
    iap.shutdown();
    sip.shutdown();
}

TEST(TCPKeepaliveTest, DeadPeerDetectedQuickly) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    {
        InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
        EXPECT_TRUE(downstream.initialize());

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT_TRUE(upstream.connect_to_downstream());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        EXPECT_EQ(upstream.downstream_state(), ConnectionState::CONNECTED);
    }

    for (int i = 0; i < 40; ++i) {
        if (upstream.downstream_state() != ConnectionState::CONNECTED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    EXPECT_NE(upstream.downstream_state(), ConnectionState::CONNECTED);

    upstream.shutdown();
}

TEST(ReconnectionTest, DataFlowsAfterMidCallReconnect) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    {
        InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
        EXPECT_TRUE(downstream.initialize());

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT_TRUE(upstream.connect_to_downstream());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        EXPECT_EQ(upstream.downstream_state(), ConnectionState::CONNECTED);

        const char* pre = "pre_disconnect";
        Packet pre_pkt(1, pre, strlen(pre));
        EXPECT_TRUE(upstream.send_to_downstream(pre_pkt));

        Packet recv;
        EXPECT_TRUE(downstream.recv_from_upstream(recv, 1000));
        EXPECT_EQ(recv.call_id, 1u);
    }

    for (int i = 0; i < 20; ++i) {
        if (upstream.downstream_state() != ConnectionState::CONNECTED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    EXPECT_NE(upstream.downstream_state(), ConnectionState::CONNECTED);

    InterconnectNode downstream2(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(downstream2.initialize());

    for (int i = 0; i < 40; ++i) {
        if (upstream.downstream_state() == ConnectionState::CONNECTED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    EXPECT_EQ(upstream.downstream_state(), ConnectionState::CONNECTED);

    const char* post = "post_reconnect";
    Packet post_pkt(2, post, strlen(post));
    EXPECT_TRUE(upstream.send_to_downstream(post_pkt));

    Packet recv2;
    EXPECT_TRUE(downstream2.recv_from_upstream(recv2, 2000));
    EXPECT_EQ(recv2.call_id, 2u);
    EXPECT_EQ(std::string(recv2.payload.begin(), recv2.payload.end()), std::string(post));

    downstream2.shutdown();
    upstream.shutdown();
}

TEST(ReconnectionTest, MiddleNodeRestartInThreeHopPipeline) {
    InterconnectNode sip(ServiceType::SIP_CLIENT);
    InterconnectNode vad(ServiceType::VAD_SERVICE);

    EXPECT_TRUE(sip.initialize());
    EXPECT_TRUE(vad.initialize());

    {
        InterconnectNode iap(ServiceType::INBOUND_AUDIO_PROCESSOR);
        EXPECT_TRUE(iap.initialize());

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT_TRUE(sip.connect_to_downstream());
        EXPECT_TRUE(iap.connect_to_downstream());
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        EXPECT_EQ(sip.downstream_state(), ConnectionState::CONNECTED);
        EXPECT_EQ(iap.downstream_state(), ConnectionState::CONNECTED);

        const char* data = "through_iap";
        Packet pkt(1, data, strlen(data));
        EXPECT_TRUE(sip.send_to_downstream(pkt));

        Packet iap_recv;
        EXPECT_TRUE(iap.recv_from_upstream(iap_recv, 1000));
        EXPECT_EQ(iap_recv.call_id, 1u);

        Packet fwd(1, data, strlen(data));
        EXPECT_TRUE(iap.send_to_downstream(fwd));

        Packet vad_recv;
        EXPECT_TRUE(vad.recv_from_upstream(vad_recv, 1000));
        EXPECT_EQ(vad_recv.call_id, 1u);
    }

    for (int i = 0; i < 20; ++i) {
        if (sip.downstream_state() != ConnectionState::CONNECTED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    EXPECT_NE(sip.downstream_state(), ConnectionState::CONNECTED);

    InterconnectNode iap2(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(iap2.initialize());

    for (int i = 0; i < 40; ++i) {
        if (sip.downstream_state() == ConnectionState::CONNECTED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    EXPECT_EQ(sip.downstream_state(), ConnectionState::CONNECTED);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(iap2.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_EQ(iap2.downstream_state(), ConnectionState::CONNECTED);

    const char* data2 = "after_iap_restart";
    Packet pkt2(3, data2, strlen(data2));
    EXPECT_TRUE(sip.send_to_downstream(pkt2));

    Packet iap2_recv;
    EXPECT_TRUE(iap2.recv_from_upstream(iap2_recv, 1000));
    EXPECT_EQ(iap2_recv.call_id, 3u);

    Packet fwd2(3, data2, strlen(data2));
    EXPECT_TRUE(iap2.send_to_downstream(fwd2));

    Packet vad2_recv;
    EXPECT_TRUE(vad.recv_from_upstream(vad2_recv, 1000));
    EXPECT_EQ(vad2_recv.call_id, 3u);
    EXPECT_EQ(std::string(vad2_recv.payload.begin(), vad2_recv.payload.end()), std::string(data2));

    iap2.shutdown();
    vad.shutdown();
    sip.shutdown();
}

TEST(PacketTraceTest, HopTracking) {
    PacketTrace trace;
    EXPECT_EQ(trace.hop_count, 0);

    trace.record(ServiceType::SIP_CLIENT, 0);
    EXPECT_EQ(trace.hop_count, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    trace.record(ServiceType::INBOUND_AUDIO_PROCESSOR, 0);
    EXPECT_EQ(trace.hop_count, 2);

    EXPECT_GE(trace.total_ms(), 5.0);
    EXPECT_GE(trace.hop_ms(1), 5.0);
}

TEST(PipelineServiceTest, FrontendIsNotPipelineService) {
    EXPECT_TRUE(is_pipeline_service(ServiceType::SIP_CLIENT));
    EXPECT_TRUE(is_pipeline_service(ServiceType::INBOUND_AUDIO_PROCESSOR));
    EXPECT_TRUE(is_pipeline_service(ServiceType::VAD_SERVICE));
    EXPECT_TRUE(is_pipeline_service(ServiceType::WHISPER_SERVICE));
    EXPECT_TRUE(is_pipeline_service(ServiceType::LLAMA_SERVICE));
    EXPECT_TRUE(is_pipeline_service(ServiceType::TTS_SERVICE));
    EXPECT_TRUE(is_pipeline_service(ServiceType::OUTBOUND_AUDIO_PROCESSOR));
    EXPECT_FALSE(is_pipeline_service(ServiceType::FRONTEND));
}

TEST(FrontendNodeTest, FrontendInitializesWithoutPorts) {
    InterconnectNode frontend(ServiceType::FRONTEND);
    EXPECT_TRUE(frontend.initialize());
    frontend.shutdown();
}

TEST(StressTest, ConcurrentCallIDs_NoMixup) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);

    EXPECT_TRUE(upstream.initialize());
    EXPECT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(upstream.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(upstream.downstream_state(), ConnectionState::CONNECTED);

    constexpr int LINES = 8;
    constexpr int PKTS_PER_LINE = 50;
    constexpr int TOTAL = LINES * PKTS_PER_LINE;

    std::atomic<int> sent_count{0};
    std::atomic<int> recv_count{0};
    std::atomic<bool> wrong_cid{false};

    std::map<uint32_t, int> cid_to_line;
    std::mutex cid_map_mutex;
    std::vector<uint32_t> cids(LINES);
    for (int i = 0; i < LINES; ++i) {
        cids[i] = upstream.reserve_call_id(100 + i);
        std::lock_guard<std::mutex> lock(cid_map_mutex);
        cid_to_line[cids[i]] = i;
    }

    std::thread receiver([&]() {
        std::map<uint32_t, int> per_cid_count;
        for (int i = 0; i < TOTAL; ++i) {
            Packet pkt;
            if (!downstream.recv_from_upstream(pkt, 10000)) {
                wrong_cid = true;
                break;
            }
            recv_count++;
            per_cid_count[pkt.call_id]++;
        }
        for (auto& [cid, count] : per_cid_count) {
            if (count != PKTS_PER_LINE) wrong_cid = true;
        }
        if ((int)per_cid_count.size() != LINES) wrong_cid = true;
    });

    std::vector<std::thread> senders;
    for (int i = 0; i < LINES; ++i) {
        senders.emplace_back([&, i]() {
            uint32_t cid = cids[i];
            for (int j = 0; j < PKTS_PER_LINE; ++j) {
                std::string data = "line" + std::to_string(i) + "_pkt" + std::to_string(j);
                Packet pkt(cid, data.c_str(), data.size());
                upstream.send_to_downstream(pkt);
                sent_count++;
            }
        });
    }

    for (auto& t : senders) t.join();
    receiver.join();

    EXPECT_EQ(sent_count.load(), TOTAL);
    EXPECT_EQ(recv_count.load(), TOTAL);
    EXPECT_FALSE(wrong_cid.load()) << "Call ID distribution was incorrect";

    downstream.shutdown();
    upstream.shutdown();
}

TEST(StressTest, BurstFlood_TenSenders_5000Packets) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);

    EXPECT_TRUE(upstream.initialize());
    EXPECT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(upstream.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    constexpr int SENDERS = 10;
    constexpr int PKTS_EACH = 500;
    constexpr int TOTAL = SENDERS * PKTS_EACH;

    std::atomic<int> recv_count{0};

    std::thread receiver([&]() {
        while (recv_count.load() < TOTAL) {
            Packet pkt;
            if (downstream.recv_from_upstream(pkt, 15000)) {
                recv_count++;
            } else {
                break;
            }
        }
    });

    std::vector<std::thread> senders;
    for (int i = 0; i < SENDERS; ++i) {
        senders.emplace_back([&, i]() {
            uint32_t cid = (uint32_t)(i + 1);
            for (int j = 0; j < PKTS_EACH; ++j) {
                std::string data = "s" + std::to_string(i) + "p" + std::to_string(j);
                Packet pkt(cid, data.c_str(), data.size());
                upstream.send_to_downstream(pkt);
            }
        });
    }

    for (auto& t : senders) t.join();
    receiver.join();

    EXPECT_EQ(recv_count.load(), TOTAL) << "Expected " << TOTAL << " packets, got " << recv_count.load();

    downstream.shutdown();
    upstream.shutdown();
}

TEST(StressTest, SimultaneousCallEnd_MultipleLines) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);

    EXPECT_TRUE(upstream.initialize());
    EXPECT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(upstream.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    constexpr int LINES = 10;
    std::atomic<int> end_count{0};

    downstream.register_call_end_handler([&](uint32_t) {
        end_count++;
    });

    std::vector<uint32_t> cids;
    for (int i = 0; i < LINES; ++i) {
        cids.push_back(upstream.reserve_call_id(200 + i));
    }

    std::vector<std::thread> enders;
    for (int i = 0; i < LINES; ++i) {
        enders.emplace_back([&, i]() {
            upstream.broadcast_call_end(cids[i]);
        });
    }
    for (auto& t : enders) t.join();

    for (int i = 0; i < 100 && end_count.load() < LINES; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_EQ(end_count.load(), LINES) << "Expected " << LINES << " CALL_END signals, got " << end_count.load();

    downstream.shutdown();
    upstream.shutdown();
}

TEST(StressTest, ThreeHopPipeline_ConcurrentMultiLine) {
    InterconnectNode sip(ServiceType::SIP_CLIENT);
    InterconnectNode iap(ServiceType::INBOUND_AUDIO_PROCESSOR);
    InterconnectNode vad(ServiceType::VAD_SERVICE);

    EXPECT_TRUE(sip.initialize());
    EXPECT_TRUE(iap.initialize());
    EXPECT_TRUE(vad.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(sip.connect_to_downstream());
    EXPECT_TRUE(iap.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    EXPECT_EQ(sip.downstream_state(), ConnectionState::CONNECTED);
    EXPECT_EQ(iap.downstream_state(), ConnectionState::CONNECTED);

    constexpr int LINES = 4;
    constexpr int PKTS_PER_LINE = 25;
    constexpr int TOTAL = LINES * PKTS_PER_LINE;

    std::atomic<int> sip_to_iap_recv{0};
    std::atomic<int> iap_to_vad_recv{0};

    std::thread iap_relay([&]() {
        for (int i = 0; i < TOTAL; ++i) {
            Packet pkt;
            if (!iap.recv_from_upstream(pkt, 10000)) break;
            sip_to_iap_recv++;
            Packet fwd(pkt.call_id, pkt.payload.data(), pkt.payload.size());
            iap.send_to_downstream(fwd);
        }
    });

    std::thread vad_receiver([&]() {
        for (int i = 0; i < TOTAL; ++i) {
            Packet pkt;
            if (!vad.recv_from_upstream(pkt, 10000)) break;
            iap_to_vad_recv++;
        }
    });

    std::vector<std::thread> senders;
    for (int i = 0; i < LINES; ++i) {
        senders.emplace_back([&, i]() {
            uint32_t cid = (uint32_t)(i + 1);
            for (int j = 0; j < PKTS_PER_LINE; ++j) {
                std::string data = "hop3_line" + std::to_string(i) + "_p" + std::to_string(j);
                Packet pkt(cid, data.c_str(), data.size());
                sip.send_to_downstream(pkt);
            }
        });
    }

    for (auto& t : senders) t.join();
    iap_relay.join();
    vad_receiver.join();

    EXPECT_EQ(sip_to_iap_recv.load(), TOTAL);
    EXPECT_EQ(iap_to_vad_recv.load(), TOTAL);

    vad.shutdown();
    iap.shutdown();
    sip.shutdown();
}

TEST(SpeechActiveTest, SpeechActiveClearedOnBroadcastCallEnd) {
    InterconnectNode node(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(node.initialize());

    uint32_t cid = node.reserve_call_id(100);

    node.broadcast_speech_signal(cid, true);
    EXPECT_TRUE(node.is_speech_active(cid));

    node.broadcast_call_end(cid);
    EXPECT_FALSE(node.is_speech_active(cid));
    EXPECT_TRUE(node.has_ended(cid));

    node.shutdown();
}

TEST(SpeechActiveTest, SpeechActiveClearedOnHandleRemoteCallEnd) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(upstream.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    uint32_t cid = upstream.reserve_call_id(200);

    std::atomic<bool> speech_propagated(false);
    downstream.register_speech_signal_handler([&](uint32_t, bool active) {
        if (active) speech_propagated = true;
    });

    upstream.broadcast_speech_signal(cid, true);
    for (int i = 0; i < 50 && !speech_propagated; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(upstream.is_speech_active(cid));
    EXPECT_TRUE(downstream.is_speech_active(cid));

    upstream.broadcast_call_end(cid);

    for (int i = 0; i < 50 && downstream.is_speech_active(cid); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_FALSE(upstream.is_speech_active(cid));
    EXPECT_FALSE(downstream.is_speech_active(cid));
    EXPECT_TRUE(upstream.has_ended(cid));

    for (int i = 0; i < 50 && !downstream.has_ended(cid); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(downstream.has_ended(cid));

    downstream.shutdown();
    upstream.shutdown();
}

TEST(SpeechActiveTest, HasEndedReturnsFalseForActiveCall) {
    InterconnectNode node(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(node.initialize());

    uint32_t cid = node.reserve_call_id(300);
    EXPECT_FALSE(node.has_ended(cid));

    node.broadcast_call_end(cid);
    EXPECT_TRUE(node.has_ended(cid));

    node.shutdown();
}

// ---------------------------------------------------------------------------
// EngineClient tests — fake dock listener on an ephemeral port.
// ---------------------------------------------------------------------------

namespace {

struct FakeDock {
    int listen_sock = -1;
    uint16_t port = 0;

    bool start() {
        listen_sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock < 0) return false;
        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // ephemeral
        if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) < 0) return false;
        socklen_t alen = sizeof(addr);
        if (getsockname(listen_sock, (sockaddr*)&addr, &alen) < 0) return false;
        port = ntohs(addr.sin_port);
        if (listen(listen_sock, 4) < 0) return false;
        return true;
    }

    int accept_one(int timeout_ms = 2000) {
        pollfd pfd = {listen_sock, POLLIN, 0};
        if (::poll(&pfd, 1, timeout_ms) <= 0) return -1;
        sockaddr_in a{}; socklen_t al = sizeof(a);
        return ::accept(listen_sock, (sockaddr*)&a, &al);
    }

    ~FakeDock() { if (listen_sock >= 0) ::close(listen_sock); }
};

static bool read_hello_line(int sock, std::string& out, int timeout_ms = 2000) {
    out.clear();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (out.size() < 1024) {
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remain <= 0) return false;
        pollfd pfd = {sock, POLLIN, 0};
        if (::poll(&pfd, 1, (int)remain) <= 0) return false;
        char ch;
        ssize_t n = ::recv(sock, &ch, 1, 0);
        if (n <= 0) return false;
        if (ch == '\n') return true;
        out.push_back(ch);
    }
    return false;
}

static bool send_all_raw(int sock, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(sock, p + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool recv_exact_raw(int sock, void* buf, size_t len, int timeout_ms) {
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (got < len) {
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remain <= 0) return false;
        pollfd pfd = {sock, POLLIN, 0};
        if (::poll(&pfd, 1, (int)remain) <= 0) return false;
        ssize_t n = ::recv(sock, p + got, len - got, 0);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

}  // namespace

TEST(EngineClientTest, HelloExchangePacketReconnect) {
    FakeDock dock;
    ASSERT_TRUE(dock.start());

    EngineClient client;
    client.set_name("kokoro-test");
    client.set_endpoint("127.0.0.1", dock.port);
    ASSERT_TRUE(client.start());

    // --- first connection ---
    int s1 = dock.accept_one();
    ASSERT_GE(s1, 0);

    std::string hello;
    ASSERT_TRUE(read_hello_line(s1, hello));
    EXPECT_NE(hello.find("\"name\":\"kokoro-test\""), std::string::npos);
    EXPECT_NE(hello.find("\"sample_rate\":24000"), std::string::npos);
    EXPECT_NE(hello.find("\"channels\":1"), std::string::npos);
    EXPECT_NE(hello.find("\"format\":\"f32le\""), std::string::npos);

    ASSERT_TRUE(send_all_raw(s1, "OK\n", 3));

    // Wait for client to flip connected.
    for (int i = 0; i < 100 && !client.is_connected(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(client.is_connected());

    // Dock -> engine: send a text Packet framed with tag 0x01.
    {
        const char* text = "hello world";
        Packet out_pkt(7, text, (uint32_t)strlen(text));
        auto body = out_pkt.serialize();
        std::vector<uint8_t> frame;
        frame.push_back(0x01);
        frame.insert(frame.end(), body.begin(), body.end());
        ASSERT_TRUE(send_all_raw(s1, frame.data(), frame.size()));
    }

    Packet rx;
    bool got = false;
    for (int i = 0; i < 100 && !got; i++)
        got = client.recv_text(rx, 20);
    ASSERT_TRUE(got);
    EXPECT_EQ(rx.call_id, 7u);
    ASSERT_EQ(rx.payload_size, (uint32_t)strlen("hello world"));
    EXPECT_EQ(0, memcmp(rx.payload.data(), "hello world", rx.payload_size));

    // Engine -> dock: send an audio Packet via send_audio.
    {
        uint8_t fake_audio[16];
        for (int i = 0; i < 16; i++) fake_audio[i] = (uint8_t)(i * 7);
        Packet audio_pkt(7, fake_audio, sizeof(fake_audio));
        EXPECT_TRUE(client.send_audio(audio_pkt));
    }

    uint8_t tag = 0;
    ASSERT_TRUE(recv_exact_raw(s1, &tag, 1, 2000));
    EXPECT_EQ(tag, (uint8_t)0x01);
    uint8_t hdr[8];
    ASSERT_TRUE(recv_exact_raw(s1, hdr, 8, 2000));
    uint32_t net_cid, net_size;
    memcpy(&net_cid, hdr, 4);
    memcpy(&net_size, hdr + 4, 4);
    EXPECT_EQ(ntohl(net_cid), 7u);
    EXPECT_EQ(ntohl(net_size), 16u);
    uint8_t payload[16];
    ASSERT_TRUE(recv_exact_raw(s1, payload, 16, 2000));
    for (int i = 0; i < 16; i++) EXPECT_EQ(payload[i], (uint8_t)(i * 7));

    // --- close and reconnect ---
    ::shutdown(s1, SHUT_RDWR);
    ::close(s1);

    for (int i = 0; i < 200 && client.is_connected(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(client.is_connected());

    int s2 = dock.accept_one(5000);
    ASSERT_GE(s2, 0);
    std::string hello2;
    ASSERT_TRUE(read_hello_line(s2, hello2));
    EXPECT_NE(hello2.find("\"name\":\"kokoro-test\""), std::string::npos);
    ASSERT_TRUE(send_all_raw(s2, "OK\n", 3));

    for (int i = 0; i < 200 && !client.is_connected(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(client.is_connected());

    ::close(s2);
    client.shutdown();
}

TEST(EngineClientTest, HelloErrorTriggersRetry) {
    FakeDock dock;
    ASSERT_TRUE(dock.start());

    EngineClient client;
    client.set_name("neutts-test");
    client.set_endpoint("127.0.0.1", dock.port);
    ASSERT_TRUE(client.start());

    int s1 = dock.accept_one();
    ASSERT_GE(s1, 0);
    std::string line;
    ASSERT_TRUE(read_hello_line(s1, line));
    ASSERT_TRUE(send_all_raw(s1, "ERR sample_rate\n", 16));
    ::shutdown(s1, SHUT_RDWR);
    ::close(s1);

    // Client must NOT be connected, and should attempt a new HELLO.
    EXPECT_FALSE(client.is_connected());
    int s2 = dock.accept_one(3000);
    ASSERT_GE(s2, 0);
    ASSERT_TRUE(read_hello_line(s2, line));
    ASSERT_TRUE(send_all_raw(s2, "OK\n", 3));
    for (int i = 0; i < 200 && !client.is_connected(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(client.is_connected());

    ::close(s2);
    client.shutdown();
}

TEST(EngineClientTest, MgmtFramesDispatchHandlers) {
    FakeDock dock;
    ASSERT_TRUE(dock.start());

    std::atomic<uint32_t> got_call_end{0};
    std::atomic<int> speech_events{0};
    std::atomic<bool> last_active{false};
    std::atomic<bool> shutdown_fired{false};

    EngineClient client;
    client.set_name("kokoro-test");
    client.set_endpoint("127.0.0.1", dock.port);
    client.register_call_end_handler([&](uint32_t cid) { got_call_end.store(cid); });
    client.register_speech_signal_handler([&](uint32_t, bool a) {
        speech_events.fetch_add(1);
        last_active.store(a);
    });
    client.register_custom_handler("SHUTDOWN", [&] { shutdown_fired.store(true); });
    ASSERT_TRUE(client.start());

    int s1 = dock.accept_one();
    ASSERT_GE(s1, 0);
    std::string line;
    ASSERT_TRUE(read_hello_line(s1, line));
    ASSERT_TRUE(send_all_raw(s1, "OK\n", 3));

    for (int i = 0; i < 100 && !client.is_connected(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(client.is_connected());

    // SPEECH_ACTIVE
    {
        uint8_t frame[6];
        frame[0] = 0x02;
        frame[1] = (uint8_t)MgmtMsgType::SPEECH_ACTIVE;
        uint32_t net_cid = htonl(42);
        memcpy(frame + 2, &net_cid, 4);
        ASSERT_TRUE(send_all_raw(s1, frame, 6));
    }
    // CALL_END
    {
        uint8_t frame[6];
        frame[0] = 0x02;
        frame[1] = (uint8_t)MgmtMsgType::CALL_END;
        uint32_t net_cid = htonl(42);
        memcpy(frame + 2, &net_cid, 4);
        ASSERT_TRUE(send_all_raw(s1, frame, 6));
    }
    // CUSTOM "SHUTDOWN"
    {
        const char* payload = "SHUTDOWN";
        uint16_t plen = (uint16_t)strlen(payload);
        std::vector<uint8_t> frame;
        frame.push_back(0x02);
        frame.push_back((uint8_t)MgmtMsgType::CUSTOM);
        uint16_t net_len = htons(plen);
        frame.insert(frame.end(), (uint8_t*)&net_len, (uint8_t*)&net_len + 2);
        frame.insert(frame.end(), payload, payload + plen);
        ASSERT_TRUE(send_all_raw(s1, frame.data(), frame.size()));
    }

    for (int i = 0; i < 200; i++) {
        if (got_call_end.load() == 42 && speech_events.load() >= 1 && shutdown_fired.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(got_call_end.load(), 42u);
    EXPECT_GE(speech_events.load(), 1);
    EXPECT_TRUE(last_active.load());
    EXPECT_TRUE(shutdown_fired.load());

    ::close(s1);
    client.shutdown();
}

TEST(MoshiServiceTest, BasePortAndTopology) {
    EXPECT_EQ(service_base_port(ServiceType::MOSHI_SERVICE), 13155);
    EXPECT_EQ(upstream_of(ServiceType::MOSHI_SERVICE), ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_EQ(downstream_of(ServiceType::MOSHI_SERVICE), ServiceType::OUTBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(is_pipeline_service(ServiceType::MOSHI_SERVICE));
    EXPECT_STREQ(service_type_to_string(ServiceType::MOSHI_SERVICE), "MOSHI_SERVICE");
}

TEST(MoshiServiceTest, PacketTraceName) {
    EXPECT_STREQ(PacketTrace::service_type_name(9), "MSH");
}

TEST(MoshiServiceTest, FIR24kOutputLength) {
    constexpr size_t in_len = 160;
    float in[in_len];
    float out[in_len * 3];
    float history[IAP_FIR_24K_CENTER] = {};

    for (size_t i = 0; i < in_len; i++) in[i] = (i % 2 == 0) ? 1.0f : -1.0f;

    size_t produced = iap_fir_upsample_frame_24k(in, in_len, out, history);
    EXPECT_EQ(produced, in_len * 3);
}

TEST(MoshiServiceTest, FIR24kZeroInputProducesZeroOutput) {
    constexpr size_t in_len = 160;
    float in[in_len] = {};
    float out[in_len * 3];
    float history[IAP_FIR_24K_CENTER] = {};

    size_t produced = iap_fir_upsample_frame_24k(in, in_len, out, history);
    EXPECT_EQ(produced, in_len * 3);
    for (size_t i = 0; i < produced; i++) {
        EXPECT_FLOAT_EQ(out[i], 0.0f) << "non-zero at index " << i;
    }
}

TEST(MoshiServiceTest, DownstreamOverrideConnectsMoshi) {
    InterconnectNode iap(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(iap.initialize());

    iap.set_downstream_override(ServiceType::MOSHI_SERVICE);

    InterconnectNode moshi(ServiceType::MOSHI_SERVICE);
    EXPECT_TRUE(moshi.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    bool connected = iap.connect_to_downstream();
    EXPECT_TRUE(connected);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(iap.downstream_state(), ConnectionState::CONNECTED);

    moshi.shutdown();
    iap.shutdown();
}

TEST(MoshiServiceTest, DownstreamOverrideBeforeInitialize) {
    InterconnectNode iap(ServiceType::INBOUND_AUDIO_PROCESSOR);
    iap.set_downstream_override(ServiceType::MOSHI_SERVICE);
    EXPECT_TRUE(iap.initialize());

    InterconnectNode moshi(ServiceType::MOSHI_SERVICE);
    EXPECT_TRUE(moshi.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(iap.connect_to_downstream());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(iap.downstream_state(), ConnectionState::CONNECTED);

    moshi.shutdown();
    iap.shutdown();
}

TEST(MoshiServiceTest, FIR24kDCPassthrough) {
    constexpr size_t in_len = 160;
    constexpr float DC = 0.75f;
    float in[in_len];
    for (size_t i = 0; i < in_len; i++) in[i] = DC;

    float out[in_len * 3];
    float history[IAP_FIR_24K_CENTER] = {};

    iap_fir_upsample_frame_24k(in, in_len, out, history);
    iap_fir_upsample_frame_24k(in, in_len, out, history);
    size_t produced = iap_fir_upsample_frame_24k(in, in_len, out, history);

    EXPECT_EQ(produced, in_len * 3);
    for (size_t i = 0; i < produced; i++) {
        EXPECT_NEAR(out[i], DC, 0.01f) << "DC mismatch at output index " << i;
    }
}

TEST(MoshiServiceTest, FIR24kCrossFrameContinuity) {
    constexpr size_t full_len = 160;
    constexpr size_t half = full_len / 2;

    float ramp[full_len];
    for (size_t i = 0; i < full_len; i++) ramp[i] = static_cast<float>(i) / full_len;

    float out_whole[full_len * 3];
    float history_whole[IAP_FIR_24K_CENTER] = {};
    size_t produced_whole = iap_fir_upsample_frame_24k(ramp, full_len, out_whole, history_whole);

    float out_split[full_len * 3];
    float history_split[IAP_FIR_24K_CENTER] = {};
    size_t p1 = iap_fir_upsample_frame_24k(ramp, half, out_split, history_split);
    size_t p2 = iap_fir_upsample_frame_24k(ramp + half, half, out_split + p1, history_split);

    EXPECT_EQ(produced_whole, full_len * 3);
    EXPECT_EQ(p1 + p2, full_len * 3);

    for (size_t i = 0; i < produced_whole; i++) {
        EXPECT_NEAR(out_split[i], out_whole[i], 1e-5f) << "continuity mismatch at output index " << i;
    }
}
