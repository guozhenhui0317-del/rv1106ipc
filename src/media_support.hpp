#pragma once

#include "log.h"
#include <stdexcept>

namespace media_detail {

/** @brief 记录清理调用前后及返回码，不在析构中抛异常。
 * @param operation 操作名称。
 * @param call 无参数 SDK 清理调用。
 * @return 无返回值；失败保留日志供定位。
 */
template <typename F> inline void cleanup(const char *operation, F call) noexcept {
    LOG_INFO("shutdown: %s begin", operation);
    const int ret = call();
    if (ret != 0) LOG_ERROR("shutdown: %s failed: %#x", operation, ret);
    else LOG_INFO("shutdown: %s done", operation);
}

/**
 * @brief 检查 Rockchip API 返回码并把失败转换为异常。
 *
 * @param[in] ret Rockchip SDK 返回码。
 * @param[in] operation 当前 SDK 操作名称。
 *
 * @throws std::runtime_error 初始化或 SDK 操作失败。
 */
inline void require_ok(int ret, const char *operation) {
    /* 构造阶段统一转成异常，外层 RAII 会按成功标志回滚此前创建的资源。 */
    if (ret != 0) {
        LOG_ERROR("%s failed: %#x", operation, ret);
        throw std::runtime_error(operation);
    }
}

} // namespace media_detail
