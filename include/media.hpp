#ifndef RV1106_CUSTOM_MEDIA_HPP
#define RV1106_CUSTOM_MEDIA_HPP

#include <memory>
#include <string>

/**
 * 整条音视频硬件链路的 RAII 入口。
 *
 * 构造顺序：ISP -> RockIVA -> RK MPI -> FFmpeg publisher -> Video -> Audio。
 * 任一步失败都会回滚已经成功的资源；析构时严格按相反顺序释放。
 * MediaPipeline 隐藏 Rockchip SDK 类型，使使用方只需要包含这个轻量头文件。
 */
class MediaRuntime final {
public:
    /**
     * @brief 创建并启动完整媒体链路。
     *
     * @param[in] iq_file_dir RKAIQ IQ 文件目录。
     *
     * @throws std::runtime_error 初始化或 SDK 操作失败。
     */
    explicit MediaRuntime(const std::string &iq_file_dir);
    /**
     * @brief 停止并释放完整媒体链路。
     */
    ~MediaRuntime();
    /**
     * @brief 禁止复制媒体运行时。
     *
     * @details 参数：另一个 MediaRuntime 引用；接口不可调用。
     */
    MediaRuntime(const MediaRuntime &) = delete;
    /**
     * @brief 禁止复制赋值媒体运行时。
     *
     * @details 参数：另一个 MediaRuntime 引用；接口不可调用。
     */
    MediaRuntime &operator=(const MediaRuntime &) = delete;

private:
    class MediaPipeline;
    std::unique_ptr<MediaPipeline> pipeline_;
};

#endif
