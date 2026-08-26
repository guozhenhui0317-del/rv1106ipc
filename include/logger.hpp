#ifndef RV1106_CUSTOM_LOGGER_HPP
#define RV1106_CUSTOM_LOGGER_HPP

#include <string>

/**
 * EasyLogger 生命周期封装。
 *
 * 构造成功后日志同时输出到 stderr 和指定文件；析构时停止并反初始化。
 * 禁止复制，避免两个对象重复关闭 EasyLogger 的全局状态。
 */
class Logger final {
public:
    explicit Logger(const std::string &path);
    ~Logger();
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    /** 设置 0=ERROR、1=WARN、2=INFO、3=DEBUG，越界值会被夹到有效范围。 */
    void configure(int level);
};

#endif
