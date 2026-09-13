#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace publishing {
/** @brief 自有编码包；端点共享只读数据，不持有 MPI 缓冲区。 */
struct Packet {
    std::shared_ptr<const std::vector<uint8_t>> data;
    std::shared_ptr<const std::string> extra;
    uint64_t pts = 0;
    bool video = false;
    bool key = false;
};

/** @brief 单端点有界音视频队列；溢出仅使该端点重新等待关键帧。 */
class Queue final {
public:
    using Clock = std::chrono::steady_clock;
    /** @brief 设置队列上限。
     * @param bytes 最大积压字节数，默认 2 MiB。
     * @param packets 最大积压包数，默认 128。
     * @param age_ms 最长积压时间，默认 1000ms。
     */
    explicit Queue(size_t bytes = 2 * 1024 * 1024, size_t packets = 128, int age_ms = 1000)
        : max_bytes_(bytes), max_packets_(packets), max_age_(age_ms) {}

    /** @brief 入队，不等待发送线程或网络；短暂互斥仅保护队列。
     * @param packet 自有包及其参数集快照。
     * @param now 入队单调时间，测试可注入。
     * @return true 已入队；false 表示等待关键帧、超限或已关闭。
     */
    bool push(const Packet &packet, Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> guard(lock_);
        if (stopped_ || !packet.data || packet.data->empty()) return false;
        const size_t size = packet.data->size();
        if (size > max_bytes_ || !max_packets_) {
            reset_locked(); ++drops_; return false;
        }
        if (bytes_ > max_bytes_ - size || items_.size() >= max_packets_ ||
            (!items_.empty() && now - items_.front().time > max_age_)) reset_locked();
        if (waiting_key_ && (!packet.video || !packet.key)) { ++drops_; return false; }
        items_.emplace_back(packet, now);
        bytes_ += size;
        waiting_key_ = false;
        wake_.notify_one();
        return true;
    }

    /** @brief 等待一个未过期的包；只有发送线程调用。
     * @param packet 接收包。
     * @param generation 接收包所属队列代数，用于取消旧连接。
     * @return false 表示已关闭；true 表示取得一个包。
     */
    bool pop(Packet &packet, unsigned &generation) {
        std::unique_lock<std::mutex> guard(lock_);
        for (;;) {
            wake_.wait(guard, [this] { return stopped_.load() || !items_.empty(); });
            if (stopped_) return false;
            if (Clock::now() - items_.front().time > max_age_) { reset_locked(); continue; }
            packet = std::move(items_.front().packet);
            bytes_ -= packet.data->size();
            items_.pop_front();
            generation = generation_.load();
            return true;
        }
    }

    /** @brief 使当前 GOP 失效，唤醒 IO 取消检查。 @return 无返回值。 */
    void reset() { std::lock_guard<std::mutex> guard(lock_); reset_locked(); }
    /** @brief 停止接受数据并唤醒消费者。 @return 无返回值。 */
    void stop() {
        std::lock_guard<std::mutex> guard(lock_);
        stopped_ = true;
        items_.clear(); bytes_ = 0;
        wake_.notify_all();
    }
    /** @brief 查询取消条件，不获取队列锁。 @param epoch IO 使用的代数。 @return 是否取消。 */
    bool cancelled(unsigned epoch) const { return stopped_.load() || generation_.load() != epoch; }
    /** @brief 查询累计丢包数，供发送线程限频记录。 @return 丢包总数。 */
    uint64_t drops() const { return drops_.load(); }
private:
    struct Item {
        Packet packet;
        Clock::time_point time;
        Item(const Packet &p, Clock::time_point t) : packet(p), time(t) {}
    };
    /** @brief 丢弃积压 GOP；调用者已持锁。 @return 无返回值。 */
    void reset_locked() {
        drops_ += items_.size();
        items_.clear(); bytes_ = 0; waiting_key_ = true;
        ++generation_;
    }
    const size_t max_bytes_, max_packets_;
    const std::chrono::milliseconds max_age_;
    size_t bytes_ = 0;
    bool waiting_key_ = true;
    std::atomic<bool> stopped_{false};
    std::atomic<unsigned> generation_{0};
    std::atomic<uint64_t> drops_{0};
    std::mutex lock_;
    std::condition_variable wake_;
    std::deque<Item> items_;
};
} // namespace publishing
