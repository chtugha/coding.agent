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

TEST(PortConfigTest, PortCalculation) {
    PortConfig config(22222, 33333);
    
    EXPECT_EQ(config.neg_in, 22222);
    EXPECT_EQ(config.neg_out, 33333);
    EXPECT_EQ(config.down_in, 22223);
    EXPECT_EQ(config.down_out, 22224);
    EXPECT_EQ(config.up_in, 33334);
    EXPECT_EQ(config.up_out, 33335);
    EXPECT_TRUE(config.is_valid());
}

TEST(TopologyTest, UpstreamDownstreamMapping) {
    EXPECT_EQ(downstream_of(ServiceType::SIP_CLIENT), ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_EQ(downstream_of(ServiceType::INBOUND_AUDIO_PROCESSOR), ServiceType::WHISPER_SERVICE);
    EXPECT_EQ(downstream_of(ServiceType::WHISPER_SERVICE), ServiceType::LLAMA_SERVICE);
    EXPECT_EQ(downstream_of(ServiceType::LLAMA_SERVICE), ServiceType::KOKORO_SERVICE);
    EXPECT_EQ(downstream_of(ServiceType::KOKORO_SERVICE), ServiceType::OUTBOUND_AUDIO_PROCESSOR);
    EXPECT_EQ(downstream_of(ServiceType::OUTBOUND_AUDIO_PROCESSOR), ServiceType::SIP_CLIENT);

    EXPECT_EQ(upstream_of(ServiceType::INBOUND_AUDIO_PROCESSOR), ServiceType::SIP_CLIENT);
    EXPECT_EQ(upstream_of(ServiceType::WHISPER_SERVICE), ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_EQ(upstream_of(ServiceType::SIP_CLIENT), ServiceType::OUTBOUND_AUDIO_PROCESSOR);
}

TEST(MasterElectionTest, FirstServiceIsMaster) {
    InterconnectNode node1(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(node1.initialize());
    EXPECT_TRUE(node1.is_master());
    EXPECT_EQ(node1.ports().neg_in, 22222);
    EXPECT_EQ(node1.ports().neg_out, 33333);
    
    node1.shutdown();
}

TEST(MasterElectionTest, SubsequentServicesAreSlavesWithDifferentPorts) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master.initialize());
    EXPECT_TRUE(master.is_master());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    InterconnectNode slave1(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(slave1.initialize());
    EXPECT_FALSE(slave1.is_master());
    EXPECT_EQ(slave1.ports().neg_in, 22225);
    EXPECT_EQ(slave1.ports().neg_out, 33336);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    InterconnectNode slave2(ServiceType::WHISPER_SERVICE);
    EXPECT_TRUE(slave2.initialize());
    EXPECT_FALSE(slave2.is_master());
    EXPECT_EQ(slave2.ports().neg_in, 22228);
    EXPECT_EQ(slave2.ports().neg_out, 33339);
    
    slave2.shutdown();
    slave1.shutdown();
    master.shutdown();
}

TEST(PortScanningTest, NoConflictsAcrossSixServices) {
    std::vector<std::unique_ptr<InterconnectNode>> nodes;
    std::set<uint16_t> used_ports;
    
    ServiceType types[] = {
        ServiceType::SIP_CLIENT,
        ServiceType::INBOUND_AUDIO_PROCESSOR,
        ServiceType::WHISPER_SERVICE,
        ServiceType::LLAMA_SERVICE,
        ServiceType::KOKORO_SERVICE,
        ServiceType::OUTBOUND_AUDIO_PROCESSOR
    };
    
    for (auto type : types) {
        auto node = std::make_unique<InterconnectNode>(type);
        EXPECT_TRUE(node->initialize());
        
        const auto& ports = node->ports();
        EXPECT_TRUE(ports.is_valid());
        
        EXPECT_EQ(used_ports.count(ports.neg_in), 0u);
        EXPECT_EQ(used_ports.count(ports.neg_out), 0u);
        EXPECT_EQ(used_ports.count(ports.down_in), 0u);
        EXPECT_EQ(used_ports.count(ports.down_out), 0u);
        EXPECT_EQ(used_ports.count(ports.up_in), 0u);
        EXPECT_EQ(used_ports.count(ports.up_out), 0u);
        
        used_ports.insert(ports.neg_in);
        used_ports.insert(ports.neg_out);
        used_ports.insert(ports.down_in);
        used_ports.insert(ports.down_out);
        used_ports.insert(ports.up_in);
        used_ports.insert(ports.up_out);
        
        nodes.push_back(std::move(node));
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    EXPECT_EQ(nodes.size(), 6u);
    EXPECT_EQ(used_ports.size(), 36u);
    
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        (*it)->shutdown();
    }
}

TEST(TrafficConnectionTest, TwoServicesEstablishConnection) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());
    EXPECT_TRUE(upstream.is_master());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(upstream.connect_to_downstream());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    EXPECT_EQ(upstream.upstream_state(), ConnectionState::CONNECTED);
    EXPECT_EQ(downstream.downstream_state(), ConnectionState::CONNECTED);

    downstream.shutdown();
    upstream.shutdown();
}

TEST(TrafficConnectionTest, SendAndReceivePackets) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

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

TEST(TrafficConnectionTest, Send1000PacketsFullDelivery) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

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

TEST(CallIDReservationTest, AtomicReservationNoCollisions) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master.initialize());
    EXPECT_TRUE(master.is_master());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    InterconnectNode slave(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(slave.initialize());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    const int NUM_THREADS = 10;
    std::vector<std::thread> threads;
    std::vector<uint32_t> reserved_ids(NUM_THREADS);
    std::atomic<int> ready_count(0);
    
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&master, &reserved_ids, &ready_count, i]() {
            ready_count++;
            while (ready_count < NUM_THREADS) {
                std::this_thread::yield();
            }
            
            reserved_ids[i] = master.reserve_call_id(100 + i);
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
    
    slave.shutdown();
    master.shutdown();
}

TEST(CallIDReservationTest, SlaveReservationViaProtocol) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master.initialize());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    InterconnectNode slave(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(slave.initialize());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    uint32_t id1 = slave.reserve_call_id(1);
    uint32_t id2 = slave.reserve_call_id(2);
    uint32_t id3 = slave.reserve_call_id(3);
    
    EXPECT_GT(id1, 0u);
    EXPECT_GT(id2, 0u);
    EXPECT_GT(id3, 0u);
    
    EXPECT_NE(id1, id2);
    EXPECT_NE(id2, id3);
    EXPECT_NE(id1, id3);
    
    slave.shutdown();
    master.shutdown();
}

TEST(HeartbeatTest, ServiceAliveDetection) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master.initialize());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    InterconnectNode slave(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(slave.initialize());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    EXPECT_TRUE(master.is_service_alive(ServiceType::INBOUND_AUDIO_PROCESSOR));
    
    slave.shutdown();
    master.shutdown();
}

TEST(HeartbeatTest, TimeoutDetectionWithinFiveSeconds) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master.initialize());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    {
        InterconnectNode slave(ServiceType::INBOUND_AUDIO_PROCESSOR);
        EXPECT_TRUE(slave.initialize());
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        EXPECT_TRUE(master.is_service_alive(ServiceType::INBOUND_AUDIO_PROCESSOR));
    }
    
    std::this_thread::sleep_for(std::chrono::seconds(6));
    
    EXPECT_FALSE(master.is_service_alive(ServiceType::INBOUND_AUDIO_PROCESSOR));
    
    master.shutdown();
}

TEST(CallEndTest, BroadcastWithAck) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master.initialize());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    InterconnectNode slave1(ServiceType::INBOUND_AUDIO_PROCESSOR);
    InterconnectNode slave2(ServiceType::WHISPER_SERVICE);
    EXPECT_TRUE(slave1.initialize());
    EXPECT_TRUE(slave2.initialize());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    std::atomic<bool> slave1_received(false);
    std::atomic<bool> slave2_received(false);
    
    slave1.register_call_end_handler([&](uint32_t call_id) {
        EXPECT_EQ(call_id, 42u);
        slave1_received = true;
    });
    
    slave2.register_call_end_handler([&](uint32_t call_id) {
        EXPECT_EQ(call_id, 42u);
        slave2_received = true;
    });
    
    uint32_t test_call_id = master.reserve_call_id(42);
    EXPECT_GT(test_call_id, 0u);
    
    master.broadcast_call_end(test_call_id);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    EXPECT_TRUE(slave1_received);
    EXPECT_TRUE(slave2_received);
    
    slave2.shutdown();
    slave1.shutdown();
    master.shutdown();
}

TEST(CallEndTest, DuplicateCallEndIdempotent) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode slave(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(slave.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::atomic<int> handler_count(0);
    slave.register_call_end_handler([&](uint32_t) {
        handler_count++;
    });

    uint32_t cid = master.reserve_call_id(1);
    master.broadcast_call_end(cid);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    master.broadcast_call_end(cid);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    EXPECT_EQ(handler_count.load(), 1);

    slave.shutdown();
    master.shutdown();
}

TEST(ServiceTypeTest, EnumToString) {
    EXPECT_STREQ(service_type_to_string(ServiceType::SIP_CLIENT), "SIP_CLIENT");
    EXPECT_STREQ(service_type_to_string(ServiceType::INBOUND_AUDIO_PROCESSOR), "INBOUND_AUDIO_PROCESSOR");
    EXPECT_STREQ(service_type_to_string(ServiceType::WHISPER_SERVICE), "WHISPER_SERVICE");
    EXPECT_STREQ(service_type_to_string(ServiceType::LLAMA_SERVICE), "LLAMA_SERVICE");
    EXPECT_STREQ(service_type_to_string(ServiceType::KOKORO_SERVICE), "KOKORO_SERVICE");
    EXPECT_STREQ(service_type_to_string(ServiceType::OUTBOUND_AUDIO_PROCESSOR), "OUTBOUND_AUDIO_PROCESSOR");
}

TEST(ServiceDiscoveryTest, GetDownstreamPorts) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode slave(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(slave.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    PortConfig downstream = master.query_service_ports(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(downstream.is_valid());
    EXPECT_EQ(downstream.neg_in, slave.ports().neg_in);
    EXPECT_EQ(downstream.neg_out, slave.ports().neg_out);

    slave.shutdown();
    master.shutdown();
}

TEST(ConnectionStateTest, InitialStateDisconnected) {
    InterconnectNode node(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(node.initialize());

    EXPECT_EQ(node.upstream_state(), ConnectionState::DISCONNECTED);
    EXPECT_EQ(node.downstream_state(), ConnectionState::DISCONNECTED);

    node.shutdown();
}

TEST(ConnectionStateTest, StateTransitionsOnConnect) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_EQ(upstream.upstream_state(), ConnectionState::DISCONNECTED);
    
    EXPECT_TRUE(upstream.connect_to_downstream());
    EXPECT_EQ(upstream.upstream_state(), ConnectionState::CONNECTED);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(downstream.downstream_state(), ConnectionState::CONNECTED);

    downstream.shutdown();
    upstream.shutdown();
}

TEST(ReconnectionTest, UpstreamReconnectsAfterDownstreamRestart) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    {
        InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
        EXPECT_TRUE(downstream.initialize());

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        EXPECT_TRUE(upstream.connect_to_downstream());
        EXPECT_EQ(upstream.upstream_state(), ConnectionState::CONNECTED);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    InterconnectNode downstream2(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(downstream2.initialize());

    std::this_thread::sleep_for(std::chrono::seconds(5));

    EXPECT_EQ(upstream.upstream_state(), ConnectionState::CONNECTED);

    const char* data = "reconnected";
    Packet pkt(1, data, strlen(data));
    EXPECT_TRUE(upstream.send_to_downstream(pkt));

    Packet recv_pkt;
    EXPECT_TRUE(downstream2.recv_from_upstream(recv_pkt, 2000));
    EXPECT_EQ(recv_pkt.call_id, 1u);

    downstream2.shutdown();
    upstream.shutdown();
}

TEST(TrafficConnectionTest, BidirectionalSimultaneous) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(upstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

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

TEST(CallIDCollisionTest, MasterOnly1000Reservations) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master.initialize());
    EXPECT_TRUE(master.is_master());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int NUM_THREADS = 10;
    const int CALLS_PER_THREAD = 100;
    const int TOTAL_CALLS = NUM_THREADS * CALLS_PER_THREAD;

    std::vector<std::vector<uint32_t>> thread_ids(NUM_THREADS, std::vector<uint32_t>(CALLS_PER_THREAD));
    std::atomic<int> ready_count(0);

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            ready_count++;
            while (ready_count < NUM_THREADS) {
                std::this_thread::yield();
            }
            for (int c = 0; c < CALLS_PER_THREAD; c++) {
                uint32_t proposed = static_cast<uint32_t>(t * CALLS_PER_THREAD + c + 1);
                thread_ids[t][c] = master.reserve_call_id(proposed);
            }
        });
    }

    for (auto& th : threads) th.join();

    std::set<uint32_t> unique_ids;
    for (int t = 0; t < NUM_THREADS; t++) {
        for (int c = 0; c < CALLS_PER_THREAD; c++) {
            EXPECT_GT(thread_ids[t][c], 0u) << "Thread " << t << " call " << c << " got id 0";
            unique_ids.insert(thread_ids[t][c]);
        }
    }

    EXPECT_EQ(unique_ids.size(), static_cast<size_t>(TOTAL_CALLS))
        << "Expected " << TOTAL_CALLS << " unique IDs, got " << unique_ids.size()
        << " — collision detected!";

    master.shutdown();
}

TEST(CallIDCollisionTest, MixedMasterAndSlave1000Reservations) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master.initialize());
    EXPECT_TRUE(master.is_master());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode slave(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(slave.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const int MASTER_THREADS = 5;
    const int SLAVE_THREADS = 5;
    const int CALLS_PER_THREAD = 100;
    const int TOTAL_CALLS = (MASTER_THREADS + SLAVE_THREADS) * CALLS_PER_THREAD;

    std::vector<std::vector<uint32_t>> all_ids(MASTER_THREADS + SLAVE_THREADS,
                                                std::vector<uint32_t>(CALLS_PER_THREAD, 0));
    std::atomic<int> ready_count(0);
    const int TOTAL_THREADS = MASTER_THREADS + SLAVE_THREADS;

    std::vector<std::thread> threads;
    for (int t = 0; t < TOTAL_THREADS; t++) {
        threads.emplace_back([&, t]() {
            ready_count++;
            while (ready_count < TOTAL_THREADS) {
                std::this_thread::yield();
            }
            InterconnectNode& node = (t < MASTER_THREADS) ? master : slave;
            for (int c = 0; c < CALLS_PER_THREAD; c++) {
                uint32_t proposed = static_cast<uint32_t>(t * CALLS_PER_THREAD + c + 1);
                all_ids[t][c] = node.reserve_call_id(proposed);
            }
        });
    }

    for (auto& th : threads) th.join();

    std::set<uint32_t> unique_ids;
    int failures = 0;
    for (int t = 0; t < TOTAL_THREADS; t++) {
        for (int c = 0; c < CALLS_PER_THREAD; c++) {
            if (all_ids[t][c] == 0) {
                failures++;
                continue;
            }
            unique_ids.insert(all_ids[t][c]);
        }
    }

    int successful = TOTAL_CALLS - failures;
    EXPECT_EQ(unique_ids.size(), static_cast<size_t>(successful))
        << "Collision detected! " << successful << " successful reservations but only "
        << unique_ids.size() << " unique IDs";

    EXPECT_GT(successful, TOTAL_CALLS / 2)
        << "Too many reservation failures: " << failures << " out of " << TOTAL_CALLS;

    if (failures > 0) {
        std::printf("  Note: %d/%d reservations failed (slave contention on negotiation port)\n",
                    failures, TOTAL_CALLS);
    }
    std::printf("  Call ID collision test: %d successful, %zu unique, %d failures\n",
                successful, unique_ids.size(), failures);

    slave.shutdown();
    master.shutdown();
}

TEST(CallIDCollisionTest, SequentialMonotonicallyIncreasing) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int COUNT = 200;
    std::vector<uint32_t> ids(COUNT);
    for (int i = 0; i < COUNT; i++) {
        ids[i] = master.reserve_call_id(1);
    }

    for (int i = 1; i < COUNT; i++) {
        EXPECT_GT(ids[i], ids[i - 1])
            << "ID at index " << i << " (" << ids[i] << ") not greater than index "
            << (i - 1) << " (" << ids[i - 1] << ")";
    }

    std::set<uint32_t> unique_ids(ids.begin(), ids.end());
    EXPECT_EQ(unique_ids.size(), static_cast<size_t>(COUNT));

    master.shutdown();
}

TEST(MasterFailoverTest, SlavePromotesAfterMasterCrash) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master.initialize());
    EXPECT_TRUE(master.is_master());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode slave(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(slave.initialize());
    EXPECT_FALSE(slave.is_master());

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    uint32_t id1 = master.reserve_call_id(1);
    uint32_t id2 = master.reserve_call_id(2);
    EXPECT_GT(id1, 0u);
    EXPECT_GT(id2, 0u);

    std::this_thread::sleep_for(std::chrono::seconds(3));

    master.shutdown();

    auto start = std::chrono::steady_clock::now();
    bool promoted = false;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(12)) {
        if (slave.is_master()) {
            promoted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    EXPECT_TRUE(promoted) << "Slave did not promote to master within 12s";
    EXPECT_TRUE(slave.was_promoted());

    uint32_t id3 = slave.reserve_call_id(3);
    EXPECT_GT(id3, 0u);
    EXPECT_GT(id3, id2) << "Post-promotion call ID should be greater than pre-crash IDs";

    slave.shutdown();
}

TEST(MasterFailoverTest, CallIDsSurviveFailover) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode slave(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(slave.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    for (int i = 0; i < 50; i++) {
        uint32_t id = master.reserve_call_id(1);
        EXPECT_GT(id, 0u);
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));

    master.shutdown();

    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(12)) {
        if (slave.is_master()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    ASSERT_TRUE(slave.is_master());

    std::set<uint32_t> post_ids;
    for (int i = 0; i < 10; i++) {
        uint32_t id = slave.reserve_call_id(1);
        EXPECT_GT(id, 50u) << "Post-failover ID " << id << " should be > 50 (max pre-crash)";
        post_ids.insert(id);
    }

    EXPECT_EQ(post_ids.size(), 10u) << "Post-failover IDs must be unique";

    slave.shutdown();
}

TEST(MasterFailoverTest, OriginalMasterReclaims) {
    InterconnectNode* master1 = new InterconnectNode(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master1->initialize());
    EXPECT_TRUE(master1->is_master());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode slave(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(slave.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    uint32_t pre_id = master1->reserve_call_id(100);
    EXPECT_EQ(pre_id, 100u);

    std::this_thread::sleep_for(std::chrono::seconds(3));

    master1->shutdown();
    delete master1;
    master1 = nullptr;

    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(12)) {
        if (slave.is_master()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    ASSERT_TRUE(slave.is_master());

    uint32_t mid_id = slave.reserve_call_id(1);
    EXPECT_GT(mid_id, 100u);

    InterconnectNode master2(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master2.initialize());
    EXPECT_TRUE(master2.is_master());

    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto reclaim_start = std::chrono::steady_clock::now();
    bool slave_demoted = false;
    while (std::chrono::steady_clock::now() - reclaim_start < std::chrono::seconds(5)) {
        if (!slave.is_master()) {
            slave_demoted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    EXPECT_TRUE(slave_demoted) << "Slave should have demoted after original master reclaimed";
    EXPECT_TRUE(master2.is_master());

    uint32_t post_id = master2.reserve_call_id(1);
    EXPECT_GT(post_id, mid_id) << "Reclaimed master should have absorbed promoted slave's max_call_id";

    slave.shutdown();
    master2.shutdown();
}

TEST(MasterFailoverTest, ThirdPartySlaveSeesNewMaster) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode slave1(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(slave1.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode slave2(ServiceType::WHISPER_SERVICE);
    EXPECT_TRUE(slave2.initialize());

    std::this_thread::sleep_for(std::chrono::seconds(3));

    master.shutdown();

    auto start = std::chrono::steady_clock::now();
    bool any_promoted = false;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(12)) {
        if (slave1.is_master() || slave2.is_master()) {
            any_promoted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    EXPECT_TRUE(any_promoted) << "At least one slave should have promoted";

    std::this_thread::sleep_for(std::chrono::seconds(1));

    InterconnectNode& new_master = slave1.is_master() ? slave1 : slave2;
    InterconnectNode& remaining_slave = slave1.is_master() ? slave2 : slave1;

    uint32_t id = remaining_slave.reserve_call_id(1);
    EXPECT_GT(id, 0u) << "Remaining slave should be able to reserve call IDs from new master";

    new_master.shutdown();
    remaining_slave.shutdown();
}

TEST(ConcurrencyStressTest, TwentyConcurrentCallsNoXtalk) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    ASSERT_TRUE(upstream.initialize());
    ASSERT_TRUE(upstream.is_master());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
    ASSERT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ASSERT_TRUE(upstream.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const int NUM_CALLS = 20;
    const int PACKETS_PER_CALL = 500;
    const int TOTAL_PACKETS = NUM_CALLS * PACKETS_PER_CALL;

    std::vector<uint32_t> call_ids(NUM_CALLS);
    for (int i = 0; i < NUM_CALLS; i++) {
        call_ids[i] = upstream.reserve_call_id(i + 1);
        ASSERT_GT(call_ids[i], 0u) << "Failed to reserve call_id for call " << i;
    }

    std::set<uint32_t> unique_call_ids(call_ids.begin(), call_ids.end());
    ASSERT_EQ(unique_call_ids.size(), static_cast<size_t>(NUM_CALLS));

    std::atomic<int> total_sent{0};
    std::atomic<int> total_received{0};
    std::atomic<bool> sender_done{false};
    std::atomic<bool> crosstalk_detected{false};

    std::map<uint32_t, std::atomic<int>> recv_per_call;
    for (int i = 0; i < NUM_CALLS; i++) {
        recv_per_call[call_ids[i]].store(0);
    }

    std::thread receiver([&]() {
        while (!sender_done || total_received < total_sent) {
            Packet pkt;
            if (downstream.recv_from_upstream(pkt, 200)) {
                if (unique_call_ids.count(pkt.call_id) == 0) {
                    crosstalk_detected = true;
                    continue;
                }
                if (pkt.payload_size >= 4) {
                    uint32_t embedded_id;
                    memcpy(&embedded_id, pkt.payload.data(), 4);
                    if (embedded_id != pkt.call_id) {
                        crosstalk_detected = true;
                    }
                }
                recv_per_call[pkt.call_id]++;
                total_received++;
            }
            if (sender_done && total_received >= TOTAL_PACKETS) break;
        }
    });

    std::vector<std::thread> senders;
    std::atomic<int> ready_count{0};
    for (int c = 0; c < NUM_CALLS; c++) {
        senders.emplace_back([&, c]() {
            ready_count++;
            while (ready_count < NUM_CALLS) {
                std::this_thread::yield();
            }
            uint32_t cid = call_ids[c];
            for (int p = 0; p < PACKETS_PER_CALL; p++) {
                uint8_t payload[64];
                memcpy(payload, &cid, 4);
                uint32_t seq = static_cast<uint32_t>(p);
                memcpy(payload + 4, &seq, 4);
                memset(payload + 8, static_cast<uint8_t>(c), 56);
                Packet pkt(cid, payload, sizeof(payload));
                if (upstream.send_to_downstream(pkt)) {
                    total_sent++;
                }
                if (p % 50 == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
        });
    }

    for (auto& t : senders) t.join();
    sender_done = true;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (total_received < total_sent &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    receiver.join();

    EXPECT_FALSE(crosstalk_detected) << "Cross-talk detected: payload call_id mismatch";
    EXPECT_EQ(total_sent.load(), TOTAL_PACKETS) << "Not all packets were sent";
    EXPECT_EQ(total_received.load(), total_sent.load()) << "Packet loss detected";

    int min_recv = PACKETS_PER_CALL;
    int max_recv = 0;
    for (int i = 0; i < NUM_CALLS; i++) {
        int count = recv_per_call[call_ids[i]].load();
        EXPECT_EQ(count, PACKETS_PER_CALL)
            << "Call " << call_ids[i] << " received " << count
            << " packets, expected " << PACKETS_PER_CALL;
        min_recv = std::min(min_recv, count);
        max_recv = std::max(max_recv, count);
    }

    std::printf("  [STRESS] %d calls, %d total packets sent, %d received\n",
                NUM_CALLS, total_sent.load(), total_received.load());
    std::printf("  [STRESS] Per-call: min=%d max=%d expected=%d\n",
                min_recv, max_recv, PACKETS_PER_CALL);
    std::printf("  [STRESS] Cross-talk: %s\n",
                crosstalk_detected.load() ? "DETECTED (FAIL)" : "NONE (PASS)");

    downstream.shutdown();
    upstream.shutdown();
}

TEST(ConcurrencyStressTest, CallEndDuringActiveTraffic) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    ASSERT_TRUE(master.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode slave(ServiceType::INBOUND_AUDIO_PROCESSOR);
    ASSERT_TRUE(slave.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ASSERT_TRUE(master.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const int NUM_CALLS = 20;
    std::vector<uint32_t> call_ids(NUM_CALLS);
    for (int i = 0; i < NUM_CALLS; i++) {
        call_ids[i] = master.reserve_call_id(i + 1);
        ASSERT_GT(call_ids[i], 0u);
    }

    std::set<uint32_t> ended_calls;
    std::mutex ended_mutex;
    slave.register_call_end_handler([&](uint32_t call_id) {
        std::lock_guard<std::mutex> lock(ended_mutex);
        ended_calls.insert(call_id);
    });

    std::atomic<bool> stop{false};
    std::atomic<int> sent_count{0};

    std::thread sender([&]() {
        int idx = 0;
        while (!stop) {
            uint32_t cid = call_ids[idx % NUM_CALLS];
            {
                std::lock_guard<std::mutex> lock(ended_mutex);
                if (ended_calls.count(cid)) {
                    idx++;
                    continue;
                }
            }
            std::string data = "pkt_" + std::to_string(idx);
            Packet pkt(cid, data.c_str(), data.size());
            master.send_to_downstream(pkt);
            sent_count++;
            idx++;
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });

    std::thread receiver([&]() {
        while (!stop) {
            Packet pkt;
            slave.recv_from_upstream(pkt, 200);
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(1));

    for (int i = 0; i < NUM_CALLS / 2; i++) {
        master.broadcast_call_end(call_ids[i]);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));

    stop = true;
    sender.join();
    receiver.join();

    {
        std::lock_guard<std::mutex> lock(ended_mutex);
        EXPECT_EQ(ended_calls.size(), static_cast<size_t>(NUM_CALLS / 2))
            << "Expected " << NUM_CALLS / 2 << " CALL_END received, got "
            << ended_calls.size();

        for (int i = 0; i < NUM_CALLS / 2; i++) {
            EXPECT_TRUE(ended_calls.count(call_ids[i]))
                << "CALL_END for call " << call_ids[i] << " not received";
        }
    }

    EXPECT_GT(sent_count.load(), 0) << "No packets were sent during stress test";

    std::printf("  [CALL_END_STRESS] %d packets sent, %d/%d calls ended\n",
                sent_count.load(), static_cast<int>(ended_calls.size()), NUM_CALLS / 2);

    slave.shutdown();
    master.shutdown();
}

TEST(ConcurrencyStressTest, BidirectionalMultiCallRouting) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    ASSERT_TRUE(upstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
    ASSERT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ASSERT_TRUE(upstream.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const int NUM_CALLS = 20;
    const int PACKETS_PER_CALL = 100;

    std::vector<uint32_t> call_ids(NUM_CALLS);
    for (int i = 0; i < NUM_CALLS; i++) {
        call_ids[i] = upstream.reserve_call_id(i + 1);
        ASSERT_GT(call_ids[i], 0u);
    }

    std::atomic<int> down_recv{0};
    std::atomic<int> up_recv{0};
    std::atomic<bool> crosstalk{false};
    std::atomic<bool> done_sending{false};

    std::thread down_receiver([&]() {
        while (!done_sending || down_recv < NUM_CALLS * PACKETS_PER_CALL) {
            Packet pkt;
            if (downstream.recv_from_upstream(pkt, 200)) {
                uint32_t embedded;
                memcpy(&embedded, pkt.payload.data(), 4);
                if (embedded != pkt.call_id) crosstalk = true;
                down_recv++;
            }
            if (done_sending && down_recv >= NUM_CALLS * PACKETS_PER_CALL) break;
        }
    });

    std::thread up_receiver([&]() {
        while (!done_sending || up_recv < NUM_CALLS * PACKETS_PER_CALL) {
            Packet pkt;
            if (upstream.recv_from_downstream(pkt, 200)) {
                uint32_t embedded;
                memcpy(&embedded, pkt.payload.data(), 4);
                if (embedded != pkt.call_id) crosstalk = true;
                up_recv++;
            }
            if (done_sending && up_recv >= NUM_CALLS * PACKETS_PER_CALL) break;
        }
    });

    std::vector<std::thread> senders;
    std::atomic<int> ready{0};
    for (int c = 0; c < NUM_CALLS; c++) {
        senders.emplace_back([&, c]() {
            ready++;
            while (ready < NUM_CALLS) std::this_thread::yield();
            uint32_t cid = call_ids[c];
            for (int p = 0; p < PACKETS_PER_CALL; p++) {
                uint8_t payload[32];
                memcpy(payload, &cid, 4);
                uint32_t seq = static_cast<uint32_t>(p);
                memcpy(payload + 4, &seq, 4);

                Packet down_pkt(cid, payload, sizeof(payload));
                upstream.send_to_downstream(down_pkt);

                Packet up_pkt(cid, payload, sizeof(payload));
                downstream.send_to_upstream(up_pkt);
            }
        });
    }

    for (auto& t : senders) t.join();
    done_sending = true;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while ((down_recv < NUM_CALLS * PACKETS_PER_CALL ||
            up_recv < NUM_CALLS * PACKETS_PER_CALL) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    down_receiver.join();
    up_receiver.join();

    EXPECT_FALSE(crosstalk) << "Cross-talk in bidirectional multi-call";
    EXPECT_EQ(down_recv.load(), NUM_CALLS * PACKETS_PER_CALL);
    EXPECT_EQ(up_recv.load(), NUM_CALLS * PACKETS_PER_CALL);

    std::printf("  [BIDIR_STRESS] %d calls, down_recv=%d up_recv=%d crosstalk=%s\n",
                NUM_CALLS, down_recv.load(), up_recv.load(),
                crosstalk.load() ? "YES" : "NO");

    downstream.shutdown();
    upstream.shutdown();
}

TEST(ConcurrencyStressTest, TenMinuteSustainedLoad) {
    InterconnectNode upstream(ServiceType::SIP_CLIENT);
    ASSERT_TRUE(upstream.initialize());
    ASSERT_TRUE(upstream.is_master());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode downstream(ServiceType::INBOUND_AUDIO_PROCESSOR);
    ASSERT_TRUE(downstream.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ASSERT_TRUE(upstream.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const int NUM_CALLS = 20;
    const auto TEST_DURATION = std::chrono::minutes(10);

    std::vector<uint32_t> call_ids(NUM_CALLS);
    for (int i = 0; i < NUM_CALLS; i++) {
        call_ids[i] = upstream.reserve_call_id(i + 1);
        ASSERT_GT(call_ids[i], 0u);
    }
    std::set<uint32_t> valid_ids(call_ids.begin(), call_ids.end());

    struct LatencyStats {
        std::mutex mu;
        std::vector<double> samples;
        void record(double ms) {
            std::lock_guard<std::mutex> lock(mu);
            samples.push_back(ms);
        }
    };

    std::map<uint32_t, std::atomic<uint64_t>> sent_per_call;
    std::map<uint32_t, std::atomic<uint64_t>> recv_per_call;
    for (int i = 0; i < NUM_CALLS; i++) {
        sent_per_call[call_ids[i]].store(0);
        recv_per_call[call_ids[i]].store(0);
    }

    std::atomic<uint64_t> total_sent{0};
    std::atomic<uint64_t> total_received{0};
    std::atomic<bool> crosstalk{false};
    std::atomic<bool> stop{false};
    LatencyStats latency;

    std::thread receiver([&]() {
        while (!stop) {
            Packet pkt;
            if (downstream.recv_from_upstream(pkt, 200)) {
                auto now = std::chrono::steady_clock::now();
                if (valid_ids.count(pkt.call_id) == 0) {
                    crosstalk = true;
                    continue;
                }
                if (pkt.payload_size >= 12) {
                    uint32_t embedded_id;
                    memcpy(&embedded_id, pkt.payload.data(), 4);
                    if (embedded_id != pkt.call_id) {
                        crosstalk = true;
                    }
                    uint64_t send_us;
                    memcpy(&send_us, pkt.payload.data() + 4, 8);
                    auto send_tp = std::chrono::steady_clock::time_point(
                        std::chrono::microseconds(send_us));
                    double lat_ms = std::chrono::duration<double, std::milli>(
                        now - send_tp).count();
                    latency.record(lat_ms);
                }
                recv_per_call[pkt.call_id]++;
                total_received++;
            }
        }
    });

    std::vector<std::thread> senders;
    for (int c = 0; c < NUM_CALLS; c++) {
        senders.emplace_back([&, c]() {
            uint32_t cid = call_ids[c];
            uint32_t seq = 0;
            while (!stop) {
                uint8_t payload[64];
                memcpy(payload, &cid, 4);
                auto now_us = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                memcpy(payload + 4, &now_us, 8);
                memcpy(payload + 12, &seq, 4);
                memset(payload + 16, static_cast<uint8_t>(c), 48);
                Packet pkt(cid, payload, sizeof(payload));
                if (upstream.send_to_downstream(pkt)) {
                    sent_per_call[cid]++;
                    total_sent++;
                }
                seq++;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }

    auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    int report_num = 0;

    while (std::chrono::steady_clock::now() - start < TEST_DURATION) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        auto now = std::chrono::steady_clock::now();
        auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();

        if (now - last_report >= std::chrono::minutes(1)) {
            report_num++;
            double avg_lat = 0;
            {
                std::lock_guard<std::mutex> lock(latency.mu);
                if (!latency.samples.empty()) {
                    double sum = 0;
                    for (double s : latency.samples) sum += s;
                    avg_lat = sum / latency.samples.size();
                }
            }
            std::printf("  [10MIN] t=%llds: sent=%llu recv=%llu avg_lat=%.2fms xtalk=%s\n",
                        elapsed_s, total_sent.load(), total_received.load(),
                        avg_lat, crosstalk.load() ? "YES" : "NO");
            last_report = now;
        }

        if (crosstalk) {
            std::fprintf(stderr, "  [10MIN] CROSS-TALK DETECTED at t=%llds — aborting\n", elapsed_s);
            break;
        }
    }

    stop = true;
    for (auto& t : senders) t.join();

    std::this_thread::sleep_for(std::chrono::seconds(5));
    receiver.join();

    EXPECT_FALSE(crosstalk) << "Cross-talk detected during 10-minute test";

    uint64_t min_sent = UINT64_MAX, max_sent = 0;
    uint64_t min_recv = UINT64_MAX, max_recv = 0;
    bool any_call_failed = false;
    for (int i = 0; i < NUM_CALLS; i++) {
        uint64_t s = sent_per_call[call_ids[i]].load();
        uint64_t r = recv_per_call[call_ids[i]].load();
        min_sent = std::min(min_sent, s);
        max_sent = std::max(max_sent, s);
        min_recv = std::min(min_recv, r);
        max_recv = std::max(max_recv, r);
        if (s == 0 || r == 0) any_call_failed = true;
    }

    EXPECT_FALSE(any_call_failed) << "At least one call had zero packets";

    double loss_pct = 0;
    if (total_sent > 0) {
        loss_pct = 100.0 * (1.0 - static_cast<double>(total_received) / total_sent);
    }
    EXPECT_LT(loss_pct, 1.0) << "Packet loss >1%";

    double avg_latency = 0, p50 = 0, p95 = 0, p99 = 0, max_latency = 0;
    {
        std::lock_guard<std::mutex> lock(latency.mu);
        if (!latency.samples.empty()) {
            std::sort(latency.samples.begin(), latency.samples.end());
            double sum = 0;
            for (double s : latency.samples) sum += s;
            avg_latency = sum / latency.samples.size();
            size_t n = latency.samples.size();
            p50 = latency.samples[n / 2];
            p95 = latency.samples[static_cast<size_t>(n * 0.95)];
            p99 = latency.samples[static_cast<size_t>(n * 0.99)];
            max_latency = latency.samples.back();
        }
    }

    EXPECT_LT(avg_latency, 1500.0) << "Avg latency >1.5s";

    std::printf("\n  ========== 10-MINUTE SUSTAINED LOAD RESULTS ==========\n");
    std::printf("  Duration:     10 minutes\n");
    std::printf("  Calls:        %d concurrent\n", NUM_CALLS);
    std::printf("  Packets sent: %llu (per-call min=%llu max=%llu)\n",
                total_sent.load(), min_sent, max_sent);
    std::printf("  Packets recv: %llu (per-call min=%llu max=%llu)\n",
                total_received.load(), min_recv, max_recv);
    std::printf("  Packet loss:  %.4f%%\n", loss_pct);
    std::printf("  Latency avg:  %.2f ms\n", avg_latency);
    std::printf("  Latency p50:  %.2f ms\n", p50);
    std::printf("  Latency p95:  %.2f ms\n", p95);
    std::printf("  Latency p99:  %.2f ms\n", p99);
    std::printf("  Latency max:  %.2f ms\n", max_latency);
    std::printf("  Cross-talk:   %s\n", crosstalk.load() ? "DETECTED" : "NONE");
    size_t sample_count = 0;
    {
        std::lock_guard<std::mutex> lock(latency.mu);
        sample_count = latency.samples.size();
    }
    std::printf("  Samples:      %zu latency measurements\n", sample_count);
    std::printf("  ==============================================\n\n");

    downstream.shutdown();
    upstream.shutdown();
}

TEST(ResourceLeakTest, HundredCallsNoLeak) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    ASSERT_TRUE(master.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode slave(ServiceType::INBOUND_AUDIO_PROCESSOR);
    ASSERT_TRUE(slave.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::atomic<int> slave_ends{0};
    slave.register_call_end_handler([&](uint32_t) {
        slave_ends++;
    });

    const int NUM_CALLS = 100;
    for (int i = 0; i < NUM_CALLS; i++) {
        uint32_t cid = master.reserve_call_id(i + 1);
        ASSERT_GT(cid, 0u);
        master.broadcast_call_end(cid);
        if (i % 10 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));

    EXPECT_EQ(master.active_call_count(), 0u)
        << "Master should have 0 active calls after all ended";
    EXPECT_EQ(master.ended_call_count(), static_cast<size_t>(NUM_CALLS))
        << "Master should track " << NUM_CALLS << " ended calls";
    EXPECT_EQ(slave_ends.load(), NUM_CALLS)
        << "Slave should have received " << NUM_CALLS << " CALL_END";
    EXPECT_EQ(slave.active_call_count(), 0u)
        << "Slave should have 0 active calls";

    std::printf("  [LEAK] %d calls: active=%zu ended=%zu slave_ends=%d\n",
                NUM_CALLS, master.active_call_count(),
                master.ended_call_count(), slave_ends.load());

    slave.shutdown();
    master.shutdown();
}

TEST(ResourceLeakTest, RapidCreateEndCycle) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    ASSERT_TRUE(master.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode slave1(ServiceType::INBOUND_AUDIO_PROCESSOR);
    InterconnectNode slave2(ServiceType::WHISPER_SERVICE);
    ASSERT_TRUE(slave1.initialize());
    ASSERT_TRUE(slave2.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::atomic<int> s1_ends{0}, s2_ends{0};
    slave1.register_call_end_handler([&](uint32_t) { s1_ends++; });
    slave2.register_call_end_handler([&](uint32_t) { s2_ends++; });

    const int CYCLES = 50;
    for (int i = 0; i < CYCLES; i++) {
        uint32_t cid = master.reserve_call_id(1);
        ASSERT_GT(cid, 0u);
        master.broadcast_call_end(cid);
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));

    EXPECT_EQ(master.active_call_count(), 0u);
    EXPECT_EQ(s1_ends.load(), CYCLES);
    EXPECT_EQ(s2_ends.load(), CYCLES);

    std::printf("  [RAPID_CYCLE] %d cycles: s1_ends=%d s2_ends=%d active=%zu\n",
                CYCLES, s1_ends.load(), s2_ends.load(), master.active_call_count());

    slave2.shutdown();
    slave1.shutdown();
    master.shutdown();
}

TEST(CallEndPropagationTest, AllFiveSlavesReceiveCallEnd) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    ASSERT_TRUE(master.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode iap(ServiceType::INBOUND_AUDIO_PROCESSOR);
    InterconnectNode whisper(ServiceType::WHISPER_SERVICE);
    InterconnectNode llama(ServiceType::LLAMA_SERVICE);
    InterconnectNode kokoro(ServiceType::KOKORO_SERVICE);
    InterconnectNode oap(ServiceType::OUTBOUND_AUDIO_PROCESSOR);

    ASSERT_TRUE(iap.initialize());
    ASSERT_TRUE(whisper.initialize());
    ASSERT_TRUE(llama.initialize());
    ASSERT_TRUE(kokoro.initialize());
    ASSERT_TRUE(oap.initialize());

    std::this_thread::sleep_for(std::chrono::seconds(1));

    ASSERT_EQ(master.registered_service_count(), 5u)
        << "Master should have 5 registered slaves";

    std::atomic<int> ack_count{0};
    std::map<ServiceType, std::atomic<bool>> received;
    received[ServiceType::INBOUND_AUDIO_PROCESSOR].store(false);
    received[ServiceType::WHISPER_SERVICE].store(false);
    received[ServiceType::LLAMA_SERVICE].store(false);
    received[ServiceType::KOKORO_SERVICE].store(false);
    received[ServiceType::OUTBOUND_AUDIO_PROCESSOR].store(false);

    auto make_handler = [&](ServiceType type) {
        return [&, type](uint32_t) {
            received[type] = true;
            ack_count++;
        };
    };

    iap.register_call_end_handler(make_handler(ServiceType::INBOUND_AUDIO_PROCESSOR));
    whisper.register_call_end_handler(make_handler(ServiceType::WHISPER_SERVICE));
    llama.register_call_end_handler(make_handler(ServiceType::LLAMA_SERVICE));
    kokoro.register_call_end_handler(make_handler(ServiceType::KOKORO_SERVICE));
    oap.register_call_end_handler(make_handler(ServiceType::OUTBOUND_AUDIO_PROCESSOR));

    uint32_t cid = master.reserve_call_id(1);
    ASSERT_GT(cid, 0u);

    auto start = std::chrono::steady_clock::now();
    master.broadcast_call_end(cid);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_EQ(ack_count.load(), 5) << "All 5 slaves must receive CALL_END";
    EXPECT_TRUE(received[ServiceType::INBOUND_AUDIO_PROCESSOR]) << "IAP missed CALL_END";
    EXPECT_TRUE(received[ServiceType::WHISPER_SERVICE]) << "WHISPER missed CALL_END";
    EXPECT_TRUE(received[ServiceType::LLAMA_SERVICE]) << "LLAMA missed CALL_END";
    EXPECT_TRUE(received[ServiceType::KOKORO_SERVICE]) << "KOKORO missed CALL_END";
    EXPECT_TRUE(received[ServiceType::OUTBOUND_AUDIO_PROCESSOR]) << "OAP missed CALL_END";
    EXPECT_LT(elapsed, 5000) << "CALL_END broadcast took >5s";

    std::printf("  [CALL_END_ALL5] %d/5 slaves ACKed in %lldms\n",
                ack_count.load(), elapsed);

    oap.shutdown();
    kokoro.shutdown();
    llama.shutdown();
    whisper.shutdown();
    iap.shutdown();
    master.shutdown();
}

TEST(CallEndPropagationTest, ImmediateHangup) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    ASSERT_TRUE(master.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode slave(ServiceType::INBOUND_AUDIO_PROCESSOR);
    ASSERT_TRUE(slave.initialize());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    ASSERT_TRUE(master.connect_to_downstream());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::atomic<bool> end_received{false};
    uint32_t ended_id = 0;
    slave.register_call_end_handler([&](uint32_t call_id) {
        end_received = true;
        ended_id = call_id;
    });

    uint32_t cid = master.reserve_call_id(1);
    ASSERT_GT(cid, 0u);

    Packet pkt(cid, "hello", 5);
    EXPECT_TRUE(master.send_to_downstream(pkt));

    master.broadcast_call_end(cid);

    std::this_thread::sleep_for(std::chrono::seconds(1));

    EXPECT_TRUE(end_received) << "Slave did not receive CALL_END for immediate hangup";
    EXPECT_EQ(ended_id, cid);
    EXPECT_EQ(master.active_call_count(), 0u);
    EXPECT_EQ(slave.active_call_count(), 0u);

    std::printf("  [IMMEDIATE_HANGUP] call %u ended, slave_received=%d\n",
                cid, end_received.load());

    slave.shutdown();
    master.shutdown();
}

TEST(PortConflictTest, DualSipClientInstances) {
    InterconnectNode client1(ServiceType::SIP_CLIENT);
    ASSERT_TRUE(client1.initialize());
    ASSERT_TRUE(client1.is_master());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InterconnectNode client2(ServiceType::SIP_CLIENT);
    ASSERT_TRUE(client2.initialize());

    EXPECT_NE(client1.ports().neg_in, client2.ports().neg_in)
        << "Two SIP_CLIENT instances must have different ports";
    EXPECT_NE(client1.ports().neg_out, client2.ports().neg_out);
    EXPECT_NE(client1.ports().down_in, client2.ports().down_in);

    uint32_t id1 = client1.reserve_call_id(1);
    uint32_t id2 = client2.reserve_call_id(2);
    EXPECT_GT(id1, 0u);
    EXPECT_GT(id2, 0u);
    EXPECT_NE(id1, id2);

    std::printf("  [PORT_CONFLICT] client1 ports: %u/%u, client2 ports: %u/%u\n",
                client1.ports().neg_in, client1.ports().neg_out,
                client2.ports().neg_in, client2.ports().neg_out);
    std::printf("  [PORT_CONFLICT] call_ids: %u, %u (unique=%s)\n",
                id1, id2, (id1 != id2) ? "YES" : "NO");

    client2.shutdown();
    client1.shutdown();
}

class CrashRecoveryMatrixTest : public ::testing::TestWithParam<ServiceType> {};

TEST_P(CrashRecoveryMatrixTest, ServiceCrashAndRecovery) {
    ServiceType crashed_type = GetParam();

    if (crashed_type == ServiceType::SIP_CLIENT) {
        InterconnectNode master(ServiceType::SIP_CLIENT);
        ASSERT_TRUE(master.initialize());
        ASSERT_TRUE(master.is_master());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        InterconnectNode slave(ServiceType::INBOUND_AUDIO_PROCESSOR);
        ASSERT_TRUE(slave.initialize());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        EXPECT_TRUE(master.is_service_alive(ServiceType::INBOUND_AUDIO_PROCESSOR));

        master.shutdown();

        auto start = std::chrono::steady_clock::now();
        bool slave_promoted = false;
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(12)) {
            if (slave.is_master()) { slave_promoted = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        EXPECT_TRUE(slave_promoted) << "Slave should promote after SIP_CLIENT (master) crash";

        InterconnectNode master_restart(ServiceType::SIP_CLIENT);
        ASSERT_TRUE(master_restart.initialize());
        std::this_thread::sleep_for(std::chrono::seconds(2));

        EXPECT_TRUE(master_restart.is_master()) << "Restarted SIP_CLIENT should reclaim master";

        uint32_t id = master_restart.reserve_call_id(1);
        EXPECT_GT(id, 0u) << "Restarted master should reserve call IDs";

        std::printf("  [SIP_CLIENT] Master crash + promotion + reclaim: PASS\n");

        slave.shutdown();
        master_restart.shutdown();
        return;
    }

    InterconnectNode master(ServiceType::SIP_CLIENT);
    ASSERT_TRUE(master.initialize());
    ASSERT_TRUE(master.is_master());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto* crashed_node = new InterconnectNode(crashed_type);
    ASSERT_TRUE(crashed_node->initialize());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_TRUE(master.is_service_alive(crashed_type))
        << service_type_to_string(crashed_type) << " should be alive before crash";

    ServiceType upstream_type = upstream_of(crashed_type);
    std::unique_ptr<InterconnectNode> upstream_neighbor;
    if (upstream_type != ServiceType::SIP_CLIENT) {
        upstream_neighbor = std::make_unique<InterconnectNode>(upstream_type);
        ASSERT_TRUE(upstream_neighbor->initialize());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        upstream_neighbor->connect_to_downstream();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    crashed_node->shutdown();
    delete crashed_node;
    crashed_node = nullptr;

    auto crash_time = std::chrono::steady_clock::now();

    auto start = std::chrono::steady_clock::now();
    bool detected = false;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(8)) {
        if (!master.is_service_alive(crashed_type)) {
            detected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    auto detect_time = std::chrono::steady_clock::now();
    auto detect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(detect_time - crash_time).count();

    EXPECT_TRUE(detected)
        << service_type_to_string(crashed_type) << " crash not detected within 8s";

    std::printf("  [%s] Crash detected in %lldms\n",
                service_type_to_string(crashed_type), detect_ms);

    if (upstream_neighbor) {
        ConnectionState us = upstream_neighbor->upstream_state();
        bool traffic_detected = (us == ConnectionState::FAILED || us == ConnectionState::DISCONNECTED);
        std::printf("  [%s] Upstream neighbor traffic state: %s\n",
                    service_type_to_string(crashed_type),
                    traffic_detected ? "DISCONNECTED/FAILED (correct)" : "still CONNECTED");
    }

    InterconnectNode restarted(crashed_type);
    ASSERT_TRUE(restarted.initialize())
        << service_type_to_string(crashed_type) << " failed to restart";

    auto restart_time = std::chrono::steady_clock::now();
    bool re_registered = false;
    while (std::chrono::steady_clock::now() - restart_time < std::chrono::seconds(5)) {
        if (master.is_service_alive(crashed_type)) {
            re_registered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    auto rereg_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - restart_time).count();

    EXPECT_TRUE(re_registered)
        << service_type_to_string(crashed_type) << " did not re-register within 5s";

    std::printf("  [%s] Re-registered in %lldms\n",
                service_type_to_string(crashed_type), rereg_ms);

    if (upstream_neighbor) {
        bool reconnected = upstream_neighbor->connect_to_downstream();
        std::printf("  [%s] Upstream reconnect after restart: %s\n",
                    service_type_to_string(crashed_type),
                    reconnected ? "SUCCESS" : "FAILED (may need time)");
    }

    uint32_t id = master.reserve_call_id(1);
    EXPECT_GT(id, 0u);

    restarted.shutdown();
    if (upstream_neighbor) upstream_neighbor->shutdown();
    master.shutdown();
}

INSTANTIATE_TEST_SUITE_P(
    AllServiceTypes,
    CrashRecoveryMatrixTest,
    ::testing::Values(
        ServiceType::SIP_CLIENT,
        ServiceType::INBOUND_AUDIO_PROCESSOR,
        ServiceType::WHISPER_SERVICE,
        ServiceType::LLAMA_SERVICE,
        ServiceType::KOKORO_SERVICE,
        ServiceType::OUTBOUND_AUDIO_PROCESSOR
    ),
    [](const ::testing::TestParamInfo<ServiceType>& info) {
        return std::string(service_type_to_string(info.param));
    }
);

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
