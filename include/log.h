#ifndef RV1106_CUSTOM_LOG_H
#define RV1106_CUSTOM_LOG_H

#include <elog.h>

#ifndef LOG_TAG
#define LOG_TAG "rkipc"
#endif

/*
 * 对官方 RKIPC 的 LOG_* 调用保持兼容，同时统一转到 EasyLogger。
 * 每个 .c/.cpp 文件可以在包含本头文件后重定义 LOG_TAG，以便日志中直接
 * 看出消息来自 media、publisher、ISP 还是 RockIVA 模块。
 */
#define LOG_ERROR(format, ...) elog_e(LOG_TAG, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...)  elog_w(LOG_TAG, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  elog_i(LOG_TAG, format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) elog_d(LOG_TAG, format, ##__VA_ARGS__)

#endif
