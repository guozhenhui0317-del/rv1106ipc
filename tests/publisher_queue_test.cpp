#include "publisher_queue.hpp"
#include <cassert>
#include <future>

/** @brief 构造自有包。 @param video 是否视频。 @param key 是否关键帧。 @param size 长度。 @return 编码包。 */
publishing::Packet packet(bool video, bool key, size_t size = 4) {
    publishing::Packet p;
    p.data = std::make_shared<const std::vector<uint8_t>>(size, 0x67);
    p.extra = std::make_shared<const std::string>("SPS/PPS");
    p.video = video; p.key = key; p.pts = 123456;
    return p;
}

/** @brief 验证资源所有权、GOP 丢弃、两路隔离、容量/时龄限制和停止唤醒。 @return 0 为通过。 */
int main() {
    using publishing::Queue;
    publishing::Packet out;
    unsigned epoch;
    Queue q(12, 3);
    assert(!q.push(packet(false, false)));
    assert(!q.push(packet(true, false)));
    auto key = packet(true, true);
    assert(q.push(key));
    assert(q.pop(out, epoch) && out.data == key.data && out.extra == key.extra && out.pts == key.pts);
    assert(q.push(packet(false, false)));
    assert(q.push(packet(true, false)));
    assert(q.push(packet(true, false)));
    assert(!q.push(packet(true, false))); // 溢出后不能从 P 帧继续。
    assert(q.cancelled(epoch));
    assert(!q.push(packet(false, false)));
    assert(q.push(key));
    assert(q.pop(out, epoch) && out.key && !q.cancelled(epoch));
    assert(!q.push(packet(true, true, 13))); // 单包超限。
    assert(q.cancelled(epoch));
    Queue count_limit(100, 1);
    assert(count_limit.push(key));
    assert(!count_limit.push(packet(false, false)));

    Queue old;
    assert(old.push(key, Queue::Clock::now() - std::chrono::seconds(2)));
    assert(!old.push(packet(false, false)));
    assert(old.push(key));
    assert(old.pop(out, epoch) && out.key);

    Queue blocked(16, 4), healthy(16, 4);
    for (unsigned i = 0; i < 200; ++i) {
        auto p = packet(true, i % 25 == 0);
        blocked.push(p); // 模拟一路网络始终阻塞。
        assert(healthy.push(p));
        assert(healthy.pop(out, epoch));
    }
    assert(blocked.drops() > 0 && healthy.drops() == 0);
    Queue sleeper;
    auto waiting = std::async(std::launch::async, [&] { return sleeper.pop(out, epoch); });
    sleeper.stop();
    assert(waiting.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(!waiting.get());
    assert(!sleeper.push(key));
}
