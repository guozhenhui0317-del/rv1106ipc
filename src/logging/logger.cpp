#include "logger.hpp"

#include <elog.h>
#include <stdexcept>

extern "C" void elog_port_set_file(const char *path);

/**
 * @brief 初始化并启动 EasyLogger。
 *
 * @param[in] path 文件路径。
 *
 * @throws std::runtime_error 初始化或 SDK 操作失败。
 */
Logger::Logger(const std::string &path) {
    /* 必须在 elog_init() 前传入文件名，因为端口初始化阶段就会打开文件。 */
    elog_port_set_file(path.c_str());
    if (elog_init() != ELOG_NO_ERR)
        throw std::runtime_error("EasyLogger initialization failed");
    /* 每条日志包含等级、模块、时间、进程和线程，方便定位多线程媒体问题。 */
    const size_t format = ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME |
                          ELOG_FMT_P_INFO | ELOG_FMT_T_INFO;
    for (int level = ELOG_LVL_ASSERT; level <= ELOG_LVL_VERBOSE; ++level)
        elog_set_fmt(static_cast<uint8_t>(level), format);
    elog_start();
    configure(2);
}

/**
 * @brief 停止并反初始化 EasyLogger。
 */
Logger::~Logger() {
    /* elog_stop() 先阻止新输出，elog_deinit() 再调用端口层关闭文件。 */
    elog_stop();
    elog_deinit();
}

/**
 * @brief 设置 EasyLogger 的输出过滤等级。
 *
 * @param[in] level 日志等级。
 */
void Logger::configure(int level) {
    /* INI 使用更直观的 0..3，下面转换为 EasyLogger 自身的等级枚举。 */
    if (level < 0)
        level = 0;
    if (level > 3)
        level = 3;
    static const uint8_t easylogger_levels[] = {
        ELOG_LVL_ERROR, ELOG_LVL_WARN, ELOG_LVL_INFO, ELOG_LVL_DEBUG};
    elog_set_filter_lvl(easylogger_levels[level]);
}
