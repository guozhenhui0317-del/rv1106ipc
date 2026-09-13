#include "media_health.hpp"
#include <cassert>
#include <cstring>

/** @brief 验证宽限期、禁用通道、进展恢复及每一路无帧检测。 @return 0 为通过。 */
int main() {
    auto &health = MediaHealth::instance();
    health.reset(false, false, 100);
    assert(!health.stalled(159));
    assert(std::strcmp(health.stalled(160), "VENC0") == 0);
    health.record(0, 160);
    assert(std::strcmp(health.stalled(160), "VENC1") == 0);
    health.record(1, 160);
    assert(!health.stalled(160));
    health.reset(true, true, 200);
    health.record(0, 260); health.record(1, 260);
    assert(std::strcmp(health.stalled(260), "AENC0") == 0);
    health.record(2, 260);
    assert(std::strcmp(health.stalled(260), "IVA") == 0);
    health.record(3, 260);
    health.record(999, 260);
    assert(!health.stalled(260));
}
