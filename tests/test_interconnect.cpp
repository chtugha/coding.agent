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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
