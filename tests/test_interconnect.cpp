#include <gtest/gtest.h>
#include "interconnect.h"
#include <thread>
#include <chrono>
#include <vector>
#include <set>
#include <atomic>

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
    EXPECT_EQ(service_base_port(ServiceType::WHISPER_SERVICE), 13120);
    EXPECT_EQ(service_base_port(ServiceType::LLAMA_SERVICE), 13130);
    EXPECT_EQ(service_base_port(ServiceType::KOKORO_SERVICE), 13140);
    EXPECT_EQ(service_base_port(ServiceType::OUTBOUND_AUDIO_PROCESSOR), 13150);
    EXPECT_EQ(service_base_port(ServiceType::FRONTEND), 13160);
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
        ServiceType::WHISPER_SERVICE,
        ServiceType::LLAMA_SERVICE,
        ServiceType::KOKORO_SERVICE,
        ServiceType::OUTBOUND_AUDIO_PROCESSOR,
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
    }
    EXPECT_EQ(all_ports.size(), 21u);
}

TEST(TopologyTest, UpstreamDownstreamMapping) {
    EXPECT_EQ(downstream_of(ServiceType::SIP_CLIENT), ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_EQ(downstream_of(ServiceType::INBOUND_AUDIO_PROCESSOR), ServiceType::VAD_SERVICE);
    EXPECT_EQ(downstream_of(ServiceType::VAD_SERVICE), ServiceType::WHISPER_SERVICE);
    EXPECT_EQ(downstream_of(ServiceType::WHISPER_SERVICE), ServiceType::LLAMA_SERVICE);
    EXPECT_EQ(downstream_of(ServiceType::LLAMA_SERVICE), ServiceType::KOKORO_SERVICE);
    EXPECT_EQ(downstream_of(ServiceType::KOKORO_SERVICE), ServiceType::OUTBOUND_AUDIO_PROCESSOR);
    EXPECT_EQ(downstream_of(ServiceType::OUTBOUND_AUDIO_PROCESSOR), ServiceType::SIP_CLIENT);

    EXPECT_EQ(upstream_of(ServiceType::INBOUND_AUDIO_PROCESSOR), ServiceType::SIP_CLIENT);
    EXPECT_EQ(upstream_of(ServiceType::VAD_SERVICE), ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_EQ(upstream_of(ServiceType::WHISPER_SERVICE), ServiceType::VAD_SERVICE);
    EXPECT_EQ(upstream_of(ServiceType::SIP_CLIENT), ServiceType::OUTBOUND_AUDIO_PROCESSOR);
}

TEST(ServiceTypeTest, EnumToString) {
    EXPECT_STREQ(service_type_to_string(ServiceType::SIP_CLIENT), "SIP_CLIENT");
    EXPECT_STREQ(service_type_to_string(ServiceType::INBOUND_AUDIO_PROCESSOR), "INBOUND_AUDIO_PROCESSOR");
    EXPECT_STREQ(service_type_to_string(ServiceType::WHISPER_SERVICE), "WHISPER_SERVICE");
    EXPECT_STREQ(service_type_to_string(ServiceType::LLAMA_SERVICE), "LLAMA_SERVICE");
    EXPECT_STREQ(service_type_to_string(ServiceType::KOKORO_SERVICE), "KOKORO_SERVICE");
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
    EXPECT_TRUE(is_pipeline_service(ServiceType::KOKORO_SERVICE));
    EXPECT_TRUE(is_pipeline_service(ServiceType::OUTBOUND_AUDIO_PROCESSOR));
    EXPECT_FALSE(is_pipeline_service(ServiceType::FRONTEND));
}

TEST(FrontendNodeTest, FrontendInitializesWithoutPorts) {
    InterconnectNode frontend(ServiceType::FRONTEND);
    EXPECT_TRUE(frontend.initialize());
    frontend.shutdown();
}
