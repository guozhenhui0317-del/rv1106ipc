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
    /**
     * @brief 创建并启动日志系统。
     *
     * @param[in] path 文件路径。
     *
     * @throws std::runtime_error 初始化或 SDK 操作失败。
     */
    explicit Logger(const std::string &path);
    /**
     * @brief 停止日志系统并释放资源。
     */
    ~Logger();
    /**
     * @brief 禁止复制 Logger。
     *
     * @details 参数：另一个 Logger 引用；接口不可调用。
     */
    Logger(const Logger &) = delete;
    /**
     * @brief 禁止复制赋值 Logger。
     *
     * @details 参数：另一个 Logger 引用；接口不可调用。
     */
    Logger &operator=(const Logger &) = delete;

    /**
     * @brief 设置日志过滤等级。
     *
     * @param[in] level 日志等级。
     */
    void configure(int level);
};

#endif
