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
    
    for (auto& node : nodes) {
        node->shutdown();
    }
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
    EXPECT_EQ(unique_ids.size(), NUM_THREADS);
    
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

TEST(CallEndTest, BroadcastAndCleanup) {
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

TEST(ServiceTypeTest, EnumToString) {
    EXPECT_STREQ(service_type_to_string(ServiceType::SIP_CLIENT), "SIP_CLIENT");
    EXPECT_STREQ(service_type_to_string(ServiceType::INBOUND_AUDIO_PROCESSOR), "INBOUND_AUDIO_PROCESSOR");
    EXPECT_STREQ(service_type_to_string(ServiceType::WHISPER_SERVICE), "WHISPER_SERVICE");
    EXPECT_STREQ(service_type_to_string(ServiceType::LLAMA_SERVICE), "LLAMA_SERVICE");
    EXPECT_STREQ(service_type_to_string(ServiceType::KOKORO_SERVICE), "KOKORO_SERVICE");
    EXPECT_STREQ(service_type_to_string(ServiceType::OUTBOUND_AUDIO_PROCESSOR), "OUTBOUND_AUDIO_PROCESSOR");
}

TEST(ReconnectionTest, DownstreamStateTracking) {
    InterconnectNode master(ServiceType::SIP_CLIENT);
    EXPECT_TRUE(master.initialize());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    InterconnectNode slave(ServiceType::INBOUND_AUDIO_PROCESSOR);
    EXPECT_TRUE(slave.initialize());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    EXPECT_EQ(slave.downstream_state(), ConnectionState::DISCONNECTED);
    
    slave.shutdown();
    master.shutdown();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
