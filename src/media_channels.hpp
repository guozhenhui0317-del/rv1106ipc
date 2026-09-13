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
class Video final {
public:
    explicit Video(VideoSink sink);
    ~Video();
    Video(const Video &) = delete;
    Video &operator=(const Video &) = delete;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class Audio final {
public:
    explicit Audio(AudioSink sink);
    ~Audio();
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace media_detail
