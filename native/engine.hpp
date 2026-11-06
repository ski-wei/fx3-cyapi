#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fx3 {
using Clock = std::chrono::steady_clock;
struct Config {
    size_t block_size = 1 << 20;
    size_t queue_depth = 8;
    size_t queue_capacity = 16;
    size_t pool_extra = 8;
    unsigned timeout_ms = 1000;
    unsigned simulated_interval_ms = 2;
    uint64_t simulated_fail_after = 0;
};
struct Buffer { explicit Buffer(size_t n): bytes(n) {} std::vector<uint8_t> bytes; };
class Pool : public std::enable_shared_from_this<Pool> {
    std::mutex mutex_;
    std::vector<std::unique_ptr<Buffer>> free_;
public:
    Pool(size_t count, size_t size);
    std::shared_ptr<Buffer> acquire();
    size_t available();
};
struct Block {
    std::shared_ptr<Buffer> buffer;
    size_t size = 0;
    uint64_t sequence = 0;
    uint64_t timestamp_ns = 0; // host monotonic completion time, not a device timestamp
};
struct Stats {
    bool opened = false, running = false;
    uint64_t received_blocks = 0, received_bytes = 0;
    uint64_t dropped_blocks = 0, dropped_bytes = 0, errors = 0;
    size_t queued_blocks = 0, queue_high_water = 0;
    size_t pool_blocks = 0, free_buffers = 0, pool_bytes = 0;
    double elapsed_seconds = 0;
    std::string last_error;
};
struct DeviceInfo {
    unsigned index = 0, vendor_id = 0, product_id = 0;
    std::string name, serial;
};
class Transport {
public:
    virtual ~Transport() = default;
    virtual size_t max_packet_size() const = 0;
    virtual void prepare(const Config&) = 0;
    virtual void submit(size_t slot, uint8_t* data, size_t size) = 0;
    virtual bool wait(size_t slot, unsigned poll_ms) = 0;
    virtual size_t finish(size_t slot) = 0;
    // Must complete every outstanding I/O before returning. No Python callbacks.
    virtual void drain() noexcept = 0;
};
std::unique_ptr<Transport> open_usb(unsigned index, unsigned endpoint);
std::vector<DeviceInfo> list_devices();
class Engine {
    std::mutex lifecycle_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::unique_ptr<Transport> transport_;
    std::shared_ptr<Pool> pool_;
    std::deque<std::shared_ptr<Block>> queue_;
    Config config_;
    Stats stats_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    bool initialized_ = false;
    Clock::time_point started_{}, ended_{};
    void run() noexcept;
    void stop_locked();
public:
    Engine() = default;
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    void open(unsigned index, unsigned endpoint, bool simulated);
    void configure(const Config&);
    void start();
    std::shared_ptr<Block> read_block(unsigned timeout_ms);
    Stats stats() const;
    void stop();
    void close();
};
}
