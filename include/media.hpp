#ifndef RV1106_CUSTOM_MEDIA_HPP
#define RV1106_CUSTOM_MEDIA_HPP

#include <memory>
#include <string>

/**
 * 整条音视频硬件链路的 RAII 入口。
 *
 * 构造顺序：ISP -> RockIVA -> RK MPI -> FFmpeg publisher -> Video -> Audio。
 * 任一步失败都会回滚已经成功的资源；析构时严格按相反顺序释放。
 * Impl 隐藏 Rockchip SDK 类型，使使用方只需要包含这个轻量头文件。
 */
class MediaRuntime final {
public:
    explicit MediaRuntime(const std::string &iq_file_dir);
    ~MediaRuntime();
    MediaRuntime(const MediaRuntime &) = delete;
    MediaRuntime &operator=(const MediaRuntime &) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
