#include "engine.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace fx3 {
Pool::Pool(size_t count, size_t size) {
    free_.reserve(count);
    for (size_t i = 0; i < count; ++i) free_.push_back(std::make_unique<Buffer>(size));
}
std::shared_ptr<Buffer> Pool::acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_.empty()) return {};
    auto ptr = free_.back().release();
    free_.pop_back();
    return {ptr, [owner = shared_from_this()](Buffer* p) {
        std::lock_guard<std::mutex> guard(owner->mutex_);
        owner->free_.emplace_back(p);
    }};
}
size_t Pool::available() { std::lock_guard<std::mutex> lock(mutex_); return free_.size(); }

class SimulatedTransport final : public Transport {
    struct Slot { uint8_t* data = nullptr; size_t size = 0; uint64_t sequence = 0; Clock::time_point due; };
    std::vector<Slot> slots_;
    Config config_;
    uint64_t submitted_ = 0, finished_ = 0;
public:
    size_t max_packet_size() const override { return 1024; }
    void prepare(const Config& c) override {
        config_ = c; slots_.assign(c.queue_depth, {}); submitted_ = finished_ = 0;
    }
    void submit(size_t i, uint8_t* data, size_t size) override {
        slots_[i] = {data, size, submitted_++, Clock::now() + std::chrono::milliseconds(config_.simulated_interval_ms)};
    }
    bool wait(size_t i, unsigned poll_ms) override {
        const auto end = std::min(slots_[i].due, Clock::now() + std::chrono::milliseconds(poll_ms));
        std::this_thread::sleep_until(end);
        return Clock::now() >= slots_[i].due;
    }
    size_t finish(size_t i) override {
        if (config_.simulated_fail_after && finished_ >= config_.simulated_fail_after)
            throw std::runtime_error("Simulated transport failure");
        auto& s = slots_[i];
        // Deterministic byte ramp; independent of the display's sample interpretation.
        for (size_t j = 0; j < s.size; ++j) s.data[j] = static_cast<uint8_t>(s.sequence + j);
        ++finished_;
        return s.size;
    }
    void drain() noexcept override { slots_.clear(); }
};
#ifndef _WIN32
std::unique_ptr<Transport> open_usb(unsigned, unsigned) {
    throw std::runtime_error("CyAPI hardware acquisition requires Windows; use simulated=True");
}
std::vector<DeviceInfo> list_devices() { return {}; }
#endif
Engine::~Engine() { close(); }
void Engine::open(unsigned index, unsigned endpoint, bool simulated) {
    std::lock_guard<std::mutex> life(lifecycle_);
    if (transport_) throw std::logic_error("Close the current device before opening another");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pool_ && pool_->available() != stats_.pool_blocks)
            throw std::logic_error("Release outstanding DataBlock/memoryview/NumPy views before reopening");
        pool_.reset();
    }
    if (index > 255 || endpoint > 255 || !(endpoint & 0x80) || !(endpoint & 0x0f))
        throw std::invalid_argument("Choose a Bulk IN endpoint (for example 0x81) and index 0..255");
    transport_ = simulated ? std::unique_ptr<Transport>(new SimulatedTransport) : open_usb(index, endpoint);
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = {}; stats_.opened = true; started_ = ended_ = {};
}
void Engine::configure(const Config& c) {
    std::lock_guard<std::mutex> life(lifecycle_);
    if (!transport_) throw std::logic_error("Open a device first");
    if (worker_.joinable()) throw std::logic_error("Call stop before configuring");
    if (!c.block_size || c.block_size > 16 * 1024 * 1024 ||
        !c.queue_depth || c.queue_depth > 64 || !c.queue_capacity || c.queue_capacity > 1024 ||
        !c.pool_extra || c.pool_extra > 1024 || !c.timeout_ms || c.timeout_ms > 60000 ||
        c.simulated_interval_ms > 60000)
        throw std::invalid_argument("Invalid transfer, queue, pool or timeout size");
    if (c.block_size % transport_->max_packet_size())
        throw std::invalid_argument("block_size must be a multiple of the selected endpoint's maximum packet size");
    const size_t count = c.queue_depth + c.queue_capacity + c.pool_extra;
    if (count > (512ull * 1024 * 1024) / c.block_size)
        throw std::invalid_argument("Configured buffer pool exceeds 512 MiB");
    // A previous generation can remain alive in Python. Refuse another pool until
    // all exported buffers are released, keeping the native memory bound meaningful.
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    if (pool_ && pool_->available() != stats_.pool_blocks)
        throw std::logic_error("Release outstanding DataBlock/memoryview/NumPy views before reconfiguring");
    pool_.reset();
    config_ = c;
}
void Engine::start() {
    std::unique_lock<std::mutex> life(lifecycle_);
    if (!transport_) throw std::logic_error("Open a device first");
    if (worker_.joinable()) throw std::logic_error("Call stop before starting again");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        if (pool_ && pool_->available() != stats_.pool_blocks)
            throw std::logic_error("Release outstanding DataBlock/memoryview/NumPy views before restarting");
        // Validate defaults against the selected endpoint too.
        if (config_.block_size % transport_->max_packet_size())
            throw std::invalid_argument("Configure a block_size matching the endpoint packet size");
        pool_.reset();
        stats_ = {}; stats_.opened = true;
        stats_.pool_blocks = config_.queue_depth + config_.queue_capacity + config_.pool_extra;
        stats_.pool_bytes = stats_.pool_blocks * config_.block_size;
        pool_ = std::make_shared<Pool>(stats_.pool_blocks, config_.block_size);
        stopping_ = false; initialized_ = false;
        started_ = Clock::now(); ended_ = started_;
        stats_.running = true;
    }
    try { worker_ = std::thread(&Engine::run, this); }
    catch (...) { std::lock_guard<std::mutex> lock(mutex_); stats_.running = false; throw; }
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return initialized_ || !stats_.running; });
    if (!stats_.last_error.empty()) {
        auto error = stats_.last_error;
        lock.unlock(); stop_locked();
        throw std::runtime_error(error);
    }
}
void Engine::run() noexcept {
    // Slots, OVERLAPPEDs and transport calls are exclusively owned by this thread.
    // Keep slot buffers alive through drain even when submission or finish throws.
    std::vector<std::shared_ptr<Buffer>> slots;
    try {
        slots.resize(config_.queue_depth);
        transport_->prepare(config_);
        std::vector<Clock::time_point> submitted(config_.queue_depth);
        for (size_t i = 0; i < slots.size(); ++i) {
            slots[i] = pool_->acquire();
            transport_->submit(i, slots[i]->bytes.data(), config_.block_size);
            submitted[i] = Clock::now();
        }
        { std::lock_guard<std::mutex> lock(mutex_); initialized_ = true; }
        ready_.notify_all();
        uint64_t sequence = 0;
        size_t i = 0;
        while (!stopping_) {
            if (!transport_->wait(i, 20)) {
                if (Clock::now() - submitted[i] >= std::chrono::milliseconds(config_.timeout_ms))
                    throw std::runtime_error("USB request timed out; acquisition stopped");
                continue;
            }
            if (stopping_) break;
            const auto size = transport_->finish(i);
            if (size > config_.block_size) throw std::runtime_error("Transport returned an invalid block length");
            const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.received_blocks; stats_.received_bytes += size;
                if (size) {
                    auto replacement = queue_.size() < config_.queue_capacity ? pool_->acquire() : nullptr;
                    if (replacement) {
                        auto block = std::make_shared<Block>();
                        block->buffer = std::move(slots[i]); block->size = size;
                        block->sequence = sequence; block->timestamp_ns = static_cast<uint64_t>(stamp);
                        queue_.push_back(std::move(block));
                        slots[i] = std::move(replacement);
                        stats_.queue_high_water = std::max(stats_.queue_high_water, queue_.size());
                    } else {
                        // Drop the newly completed block; never overwrite an exported view.
                        ++stats_.dropped_blocks; stats_.dropped_bytes += size;
                    }
                }
            }
            ++sequence;
            ready_.notify_all();
            if (stopping_) break;
            transport_->submit(i, slots[i]->bytes.data(), config_.block_size);
            submitted[i] = Clock::now();
            i = (i + 1) % slots.size();
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(mutex_); ++stats_.errors; stats_.last_error = e.what();
    } catch (const char* e) {
        std::lock_guard<std::mutex> lock(mutex_); ++stats_.errors; stats_.last_error = e;
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_); ++stats_.errors; stats_.last_error = "Unknown native acquisition error";
    }
    transport_->drain(); // no buffer may be freed before this returns
    slots.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.running = false; ended_ = Clock::now();
    }
    ready_.notify_all();
}
std::shared_ptr<Block> Engine::read_block(unsigned timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !queue_.empty() || !stats_.running; });
    if (!queue_.empty()) { auto b = queue_.front(); queue_.pop_front(); return b; }
    if (!stats_.last_error.empty()) throw std::runtime_error(stats_.last_error);
    return {};
}
Stats Engine::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto s = stats_;
    s.queued_blocks = queue_.size(); s.free_buffers = pool_ ? pool_->available() : 0;
    if (started_ != Clock::time_point{})
        s.elapsed_seconds = std::chrono::duration<double>((s.running ? Clock::now() : ended_) - started_).count();
    return s;
}
void Engine::stop_locked() {
    stopping_ = true;
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}
void Engine::stop() { std::lock_guard<std::mutex> life(lifecycle_); stop_locked(); }
void Engine::close() {
    std::lock_guard<std::mutex> life(lifecycle_);
    stop_locked();
    transport_.reset();
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    // Keep the pool reference so reopen/start can account for retained Python views.
    stats_.opened = false;
}
}
