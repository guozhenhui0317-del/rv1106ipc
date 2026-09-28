#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace media_detail {

// 回调在采集线程同步执行，不得抛异常。数据只在回调期间有效；
// 若需要延迟使用，接收方必须复制。返回值沿用 0 成功、负数失败；
// 单次输出失败不会停止采集。所有采集线程停止后才能销毁接收方。
using VideoSink = int (*)(int, const void *, size_t, uint64_t, int);
using AudioSink = int (*)(const void *, size_t, uint64_t);

// 分别拥有视频和音频通道，析构时先停止线程，再释放硬件资源。
// SDK 类型只存在于实现文件中；采集模块不依赖具体网络协议。
/** @brief 拥有两路 VI/VENC、可选 RockIVA/RGN 以及相关采集线程。 */
class Video final {
public:
    /**
     * @brief 创建视频硬件链路并启动视频相关线程。
     * @param[in] sink 编码视频接收函数；必须有效且不得抛出异常。
     * @throws std::runtime_error 配置、SDK 或线程初始化失败。
     */
    explicit Video(VideoSink sink);
    /** @brief 停止线程并反序释放视频资源。 */
    ~Video();
    /** @brief 禁止复制唯一的视频资源所有者。 */
    Video(const Video &) = delete;
    /** @brief 禁止复制赋值唯一的视频资源所有者。 */
    Video &operator=(const Video &) = delete;
private:
    /** @brief 隐藏只在实现文件中使用的 Rockchip SDK 类型。 */
    class VideoPipeline;
    std::unique_ptr<VideoPipeline> pipeline_;
};

/** @brief 拥有 AI0、可选 VQE、AENC0 以及音频取流线程。 */
class Audio final {
public:
    /**
     * @brief 创建音频硬件链路并启动音频取流线程。
     * @param[in] sink G711A 接收函数；必须有效且不得抛出异常。
     * @throws std::runtime_error 配置、SDK 或线程初始化失败。
     */
    explicit Audio(AudioSink sink);
    /** @brief 停止线程并反序释放音频资源。 */
    ~Audio();
    /** @brief 禁止复制唯一的音频资源所有者。 */
    Audio(const Audio &) = delete;
    /** @brief 禁止复制赋值唯一的音频资源所有者。 */
    Audio &operator=(const Audio &) = delete;
private:
    /** @brief 隐藏只在实现文件中使用的 Rockchip SDK 类型。 */
    class AudioPipeline;
    std::unique_ptr<AudioPipeline> pipeline_;
};

} // namespace media_detail
