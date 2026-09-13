#include <assert.h>
#include <stddef.h>

static int attempts, failures, warnings;
static int rk_signal_wait(void *signal, int timeout) {
    assert(signal != NULL && timeout == 10000);
    return attempts++ < failures ? -1 : 0;
}
#define LOG_ERROR(...) (++warnings)
#include "../common/rockiva/frame_wait.h"

/** @brief 模拟超时/中断，验证通知到达前不能返回并归还帧。 */
int main(void) {
    for (failures = 0; failures < 4; ++failures) {
        attempts = warnings = 0;
        rkipc_wait_frame_release(&attempts);
        assert(attempts == failures + 1);
        assert(warnings == failures);
    }
    return 0;
}
