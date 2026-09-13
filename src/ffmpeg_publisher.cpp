#include "ffmpeg_publisher.h"
#include "annexb.hpp"
#include "network_route.hpp"
#include "publisher_queue.hpp"
#include <array>
#include <thread>
#include <sys/prctl.h>
#include <atomic>
#include <stdexcept>

#include "log.h"
extern "C" {
#include "param.h"
}

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "publisher"

namespace {

std::atomic<bool> g_stopping{false};

using annexb::ParameterSets;
using annexb::parameter_sets;

/**
 * @brief 格式化数据开头最多 16 字节。
 *
 * @param[in] data 待处理数据的首地址。
 * @param[in] size 待处理数据的字节数。
 *
 * @return 十六进制字符串。
 */
std::string hex_prefix(const void *data, size_t size) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    const size_t count = std::min<size_t>(size, 16);
    char text[16 * 3 + 1];
    size_t pos = 0;
    for (size_t i = 0; i < count; ++i)
        pos += static_cast<size_t>(snprintf(text + pos, sizeof(text) - pos,
                                            i ? " %02x" : "%02x", bytes[i]));
    return std::string(text, pos);
}

/**
 * @brief 读取 video.N 字符串配置。
 *
 * @param[in] id 媒体通道编号。
 * @param[in] field INI 配置字段名。
 * @param[in] fallback 配置项不存在时使用的缺省值。
 *
 * @return 配置字符串指针。
 */
const char *video_text(int id, const char *field, const char *fallback) {
    /* 统一拼接 video.<通道>:<字段>，避免各调用点散落硬编码键名。 */
    char key[64];
    snprintf(key, sizeof(key), "video.%d:%s", id, field);
    return rk_param_get_string(key, fallback);
}

/**
 * @brief 把通道编码配置转换为 FFmpeg 编码 ID。
 *
 * @param[in] id 媒体通道编号。
 *
 * @return  H.264 或 HEVC 编码 ID。
 */
AVCodecID video_codec(int id) {
    const char *name = video_text(id, "output_data_type", "H.264");
    return !strcmp(name, "H.265") ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
}

/**
 * @brief 用内置数据验证参数集解析器。
 *
 * @return true 表示自检通过，false 表示失败。
 */
bool publisher_self_check() {
    /*
     * 最小启动自检：构造 SPS、PPS、IDR，期望解析器只返回前两个 NAL。
     * 若后续修改扫描边界导致回归，设备会在初始化时明确报错，而不是生成
     * 一个 VLC 无法解码、但网络连接看似正常的流。
     */
    static const uint8_t h264[] = {
        0, 0, 0, 1, 0x67, 0x64, 0x00, 0x1f,
        0, 0, 0, 1, 0x68, 0xee, 0x3c, 0x80,
        0, 0, 0, 1, 0x65, 0x88};
    const ParameterSets extra = parameter_sets(h264, sizeof(h264), false);
    return extra.complete(false) && extra.data.size() == 16 &&
           !memcmp(extra.data.data(), h264, 16) &&
           !parameter_sets(h264, 8, false).complete(false);
}

/**
 * @brief 读取 video.N 整数配置。
 *
 * @param[in] id 媒体通道编号。
 * @param[in] field INI 配置字段名。
 * @param[in] fallback 配置项不存在时使用的缺省值。
 *
 * @return 配置值或 fallback。
 */
int video_value(int id, const char *field, int fallback) {
    char key[64];
    snprintf(key, sizeof(key), "video.%d:%s", id, field);
    return rk_param_get_int(key, fallback);
}

/**
 * @brief 转发 FFmpeg 警告和错误。
 *
 * @param[in] level 日志等级。
 * @param[in] fmt FFmpeg 日志格式字符串。
 * @param[in] args FFmpeg 日志可变参数列表。
 */
void ffmpeg_log(void *, int level, const char *fmt, va_list args) {
    /* FFmpeg debug/info 输出量很大；正常状态由本模块记录，只转发警告和错误。 */
    if (level > AV_LOG_WARNING)
        return;
    char line[512];
    vsnprintf(line, sizeof(line), fmt, args);
    const size_t len = strlen(line);
    if (len && line[len - 1] == '\n')
        line[len - 1] = '\0';
    if (level <= AV_LOG_ERROR)
        LOG_ERROR("FFmpeg: %s", line);
    else
        LOG_WARN("FFmpeg: %s", line);
}

struct EndpointConfig {
    /* 一项对应一个独立 muxer/网络连接；主流和 AI 流不能共享 AVFormatContext。 */
    std::string name;
    std::string url;
    std::string format;
    int stream_id;
    bool audio;
};

class Output final {
public:
    /**
     * @brief 保存一个网络端点配置。
     *
     * @param[in] config 网络推流端点配置。
     */
    explicit Output(EndpointConfig config, NetworkRoute *network, const publishing::Queue *queue)
        : config_(std::move(config)), network_(network), queue_(queue),
          timeout_ms_(std::max(100, std::min(60000,
              rk_param_get_int("stream:io_timeout_ms", 5000)))) {}
    /**
     * @brief 关闭网络输出并释放 FFmpeg 上下文。
     */
    ~Output() { close(); }

    /** @brief 丢包后重建该端点，并应用采集侧保留的参数集。
     * @param epoch 本包队列代数。
     * @param extra 参数集快照，即使首个关键帧被丢弃也保留。
     * @return 无返回值；仅发送线程调用。
     */
    void prepare(unsigned epoch, const std::shared_ptr<const std::string> &extra) {
        if (queue_epoch_ != epoch) { close(false); queue_epoch_ = epoch; }
        if (extra && !extra->empty() && codec_extra_ != *extra) codec_extra_ = *extra;
    }

    /**
     * @brief 缓存参数集、按需建连并写入视频。
     *
     * @param[in] data 待处理数据的首地址。
     * @param[in] size 待处理数据的字节数。
     * @param[in] pts_us Rockchip 硬件产生的微秒时间戳。
     * @param[in] key 是否为视频关键帧。
     *
     * @return 0 表示成功或等待，-1 表示失败。
     */
    int write_video(const void *data, size_t size, uint64_t pts_us, bool key) {
        /*
         * 同一 Output 的音视频由唯一发送线程串行处理，不在采集线程访问
         * AVFormatContext。未连接或断线后只在关键帧上打开，保证接收端从
         * 完整 GOP 开始解码；3 秒退避避免服务器离线时高速重复连接刷日志。
         */
        if (key) {
            const ParameterSets current = parameter_sets(static_cast<const uint8_t *>(data), size,
                                                         video_codec(config_.stream_id) == AV_CODEC_ID_HEVC);
            if (current.complete(video_codec(config_.stream_id) == AV_CODEC_ID_HEVC) &&
                current.data != codec_extra_) {
                codec_extra_ = current.data;
                LOG_INFO("%s cached complete parameter sets: mask=%#x size=%zu",
                         config_.name.c_str(), current.found, codec_extra_.size());
            } else if (codec_extra_.empty() && incomplete_key_logs_++ < 3) {
                LOG_WARN("%s drops incomplete key frame: mask=%#x size=%zu head=%s",
                         config_.name.c_str(), current.found, size,
                         hex_prefix(data, size).c_str());
            }
        }
        // 即使网络离线，也先缓存首帧参数集；编码器后续关键帧可能不重复 SPS/PPS。
        if (!sync_route()) return 0;
        if (!ready_) {
            if (!key || codec_extra_.empty() ||
                std::chrono::steady_clock::now() < retry_after_)
                return 0;
            if (open(size, pts_us) != 0) {
                retry_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                return -1;
            }
        }
        return write_packet(video_, data, size, pts_us, key,
                            video_value(config_.stream_id, "dst_frame_rate_den", 1));
    }

    /**
     * @brief 在端点就绪后写入 G711A 音频。
     *
     * @param[in] data 待处理数据的首地址。
     * @param[in] size 待处理数据的字节数。
     * @param[in] pts_us Rockchip 硬件产生的微秒时间戳。
     *
     * @return 0 表示成功或丢弃，-1 表示失败。
     */
    int write_audio(const void *data, size_t size, uint64_t pts_us) {
        /*
         * 视频关键帧定义整个端点的时间原点。它到达以前不能写音频，否则
         * muxer 没有 header；早于原点的音频也必须丢弃以防无符号减法下溢。
         */
        if (!sync_route() || !ready_ || !audio_ || pts_us < origin_us_)
            return 0;
        return write_packet(audio_, data, size, pts_us, false,
                            static_cast<int>(size) / std::max(1, audio_->codecpar->channels));
    }

private:
    /**
     * @brief 检查退出、超时或路由变化，中断 FFmpeg 阻塞 IO。
     * @param opaque 当前 Output；FFmpeg 同步调用，不获取音视频锁。
     * @return 非零表示取消本次 IO。
     */
    static int interrupt(void *opaque) {
        auto &self = *static_cast<Output *>(opaque);
        return g_stopping.load() || self.queue_->cancelled(self.queue_epoch_) ||
               std::chrono::steady_clock::now() >= self.deadline_ ||
               (self.network_ && (!self.network_->ready() ||
                 self.network_->generation() != self.network_generation_));
    }

    /** @brief 为一次连接或写包设置有限的整体超时。 */
    void arm_deadline() {
        deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
    }

    /** @brief 在独立发送线程内使旧连接失效。 @return 当前是否允许推流。 */
    bool sync_route() {
        if (g_stopping) return false;
        if (!network_) return true;
        const unsigned generation = network_->generation();
        if (generation != network_generation_) {
            const bool had_connection = context_ != nullptr;
            close(false);
            network_generation_ = generation;
            if (had_connection)
                retry_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        }
        return network_->ready();
    }

    /**
     * @brief 创建输出上下文、媒体轨并连接服务器。
     *
     * @param[in] first_key_size 首个视频关键帧的字节数，仅用于调试日志。
     * @param[in] pts_us Rockchip 硬件产生的微秒时间戳。
     *
     * @return 0 表示成功，-1 表示失败。
     */
    int open(size_t first_key_size, uint64_t pts_us) {
        /* 每次重连都从全新的 context 开始，旧 socket/stream 状态不能复用。 */
        close();
        int ret = avformat_alloc_output_context2(&context_, nullptr, config_.format.c_str(),
                                                 config_.url.c_str());
        if (ret < 0 || !context_) {
            LOG_ERROR("%s: cannot allocate output context (%d)", config_.name.c_str(), ret);
            return -1;
        }

        context_->interrupt_callback = AVIOInterruptCB{interrupt, this};
        arm_deadline();

        /* 这里只做封装，不做软件编码：codecpar 描述 VENC 已编码好的码流。 */
        video_ = avformat_new_stream(context_, nullptr);
        if (!video_)
            return fail("cannot create video stream", AVERROR(ENOMEM));
        AVCodecParameters *vp = video_->codecpar;
        vp->codec_type = AVMEDIA_TYPE_VIDEO;
        vp->codec_id = video_codec(config_.stream_id);
        vp->width = video_value(config_.stream_id, "width", config_.stream_id ? 640 : 1920);
        vp->height = video_value(config_.stream_id, "height", config_.stream_id ? 360 : 1080);
        vp->bit_rate = static_cast<int64_t>(video_value(config_.stream_id, "max_rate", 2048)) * 1000;
        video_->time_base = AVRational{1, 90000};
        video_->avg_frame_rate = AVRational{
            video_value(config_.stream_id, "dst_frame_rate_num", 25),
            video_value(config_.stream_id, "dst_frame_rate_den", 1)};

        /* 首次取得后缓存参数集，断线时只需等下一关键帧，不要求编码器再次重复参数集。 */
        vp->extradata = static_cast<uint8_t *>(
            av_mallocz(codec_extra_.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!vp->extradata)
            return fail("cannot allocate codec extradata", AVERROR(ENOMEM));
        memcpy(vp->extradata, codec_extra_.data(), codec_extra_.size());
        vp->extradata_size = static_cast<int>(codec_extra_.size());

        if (config_.audio) {
            /*
             * 主流保留官方 Demo 的 G711A：AENC 已完成压缩，FFmpeg 仅复用
             * PCM_ALAW codec id 封装。AI 两个端点的 audio=false，不创建音轨。
             */
            audio_ = avformat_new_stream(context_, nullptr);
            if (!audio_)
                return fail("cannot create audio stream", AVERROR(ENOMEM));
            AVCodecParameters *ap = audio_->codecpar;
            ap->codec_type = AVMEDIA_TYPE_AUDIO;
            ap->codec_id = AV_CODEC_ID_PCM_ALAW;
            ap->sample_rate = rk_param_get_int("audio.0:sample_rate", 8000);
            ap->channels = rk_param_get_int("audio.0:channels", 1);
            ap->channel_layout = ap->channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
            ap->bits_per_coded_sample = 8;
            ap->block_align = ap->channels;
            ap->frame_size = rk_param_get_int("audio.0:frame_size", 1152);
            ap->bit_rate = static_cast<int64_t>(ap->sample_rate) * ap->channels * 8;
            audio_->time_base = AVRational{1, ap->sample_rate};
        }

        /* RTSP 默认 TCP，有线和 Wi-Fi 共用；整体 IO 截止时间由 interrupt 限制。 */
        AVDictionary *options = nullptr;
        if (config_.format == "rtsp")
            av_dict_set(&options, "rtsp_transport",
                        rk_param_get_string("stream:rtsp_transport", "tcp"), 0);
        const int timeout_ms = timeout_ms_;
        av_dict_set_int(&options, "rw_timeout", static_cast<int64_t>(timeout_ms) * 1000, 0);
        if (config_.format == "rtsp")
            av_dict_set_int(&options, "stimeout", static_cast<int64_t>(timeout_ms) * 1000, 0);
        if (!(context_->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open2(&context_->pb, config_.url.c_str(), AVIO_FLAG_WRITE,
                             &context_->interrupt_callback, &options);
            if (ret < 0) {
                av_dict_free(&options);
                return fail("cannot connect", ret);
            }
        }
        ret = avformat_write_header(context_, &options);
        av_dict_free(&options);
        if (ret < 0)
            return fail("cannot write header", ret);
        /*
         * VENC 和 AENC 的 PTS 都来自 RK MPI 的微秒时钟。记录首个视频 PTS，
         * 两类媒体统一减去它再 rescale，保留真实采集间隔并实现音视频同步。
         */
        origin_us_ = pts_us;
        ready_ = true;
        LOG_INFO("%s publishing to %s", config_.name.c_str(), config_.url.c_str());
        if (config_.format == "flv")
            LOG_INFO("%s RTMP header: key_size=%zu extra=%zu video_tb=%d/%d audio=%d",
                     config_.name.c_str(), first_key_size, codec_extra_.size(),
                     video_->time_base.num, video_->time_base.den, audio_ ? 1 : 0);
        return 0;
    }

    /**
     * @brief 复制数据到 AVPacket、换算 PTS 并写入网络。
     *
     * @param[in] stream 目标 FFmpeg 媒体轨道。
     * @param[in] data 待处理数据的首地址。
     * @param[in] size 待处理数据的字节数。
     * @param[in] pts_us Rockchip 硬件产生的微秒时间戳。
     * @param[in] key 是否为视频关键帧。
     * @param[in] duration_hint 数据包持续时间提示。
     *
     * @return 0 表示成功，-1 表示失败。
     */
    int write_packet(AVStream *stream, const void *data, size_t size, uint64_t pts_us,
                     bool key, int duration_hint) {
        /*
         * av_new_packet 分配带 AV_INPUT_BUFFER_PADDING_SIZE 的 FFmpeg 自有缓冲区。
         * 队列包虽有自己的生命周期，仍复制给 FFmpeg，以满足其引用和尾部填充要求，
         * 不能让 AVPacket 持有指向 Rockchip 缓冲区的悬空指针。
         */
        AVPacket packet;
        av_init_packet(&packet);
        packet.data = nullptr;
        packet.size = 0;
        int ret = av_new_packet(&packet, static_cast<int>(size));
        if (ret < 0) {
            LOG_ERROR("%s packet allocation failed: %s", config_.name.c_str(),
                      error_text(ret).c_str());
            return -1;
        }
        memcpy(packet.data, data, size);
        packet.stream_index = stream->index;
        /* 当前编码配置不输出需要重排的 B 帧，因此 DTS 与 PTS 相同。 */
        packet.pts = packet.dts = av_rescale_q(static_cast<int64_t>(pts_us - origin_us_),
                                              AVRational{1, 1000000}, stream->time_base);
        if (stream == video_) {
            /* duration 只作为 muxer 提示；真正同步基准仍是硬件 PTS。 */
            const int num = video_value(config_.stream_id, "dst_frame_rate_num", 25);
            const int den = video_value(config_.stream_id, "dst_frame_rate_den", 1);
            packet.duration = av_rescale_q(1, AVRational{den, num}, stream->time_base);
        } else {
            packet.duration = av_rescale_q(duration_hint,
                                           AVRational{1, audio_->codecpar->sample_rate},
                                           stream->time_base);
        }
        if (key)
            packet.flags |= AV_PKT_FLAG_KEY;
        unsigned &debug_count = stream == video_ ? video_debug_packets_ : audio_debug_packets_;
        if (config_.format == "flv" && debug_count++ < 6)
            LOG_INFO("%s RTMP input %s: size=%zu pts_us=%llu pts=%lld duration=%lld key=%d head=%s",
                     config_.name.c_str(), stream == video_ ? "video" : "audio", size,
                     static_cast<unsigned long long>(pts_us), static_cast<long long>(packet.pts),
                     static_cast<long long>(packet.duration), key ? 1 : 0,
                     hex_prefix(data, size).c_str());
        /* interleaved 接口按时间戳交织主流音视频；AI 流只有视频，行为相同。 */
        arm_deadline();
        ret = av_interleaved_write_frame(context_, &packet);
        av_packet_unref(&packet);
        if (ret < 0) {
            LOG_ERROR("%s write failed: %s", config_.name.c_str(), error_text(ret).c_str());
            close(false);
            retry_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            return -1;
        }
        return 0;
    }

    /**
     * @brief 记录失败原因并关闭端点。
     *
     * @param[in] what 失败操作的文字说明。
     * @param[in] error FFmpeg 错误码。
     *
     * @return 固定返回 -1。
     */
    int fail(const char *what, int error) {
        LOG_ERROR("%s %s: %s", config_.name.c_str(), what, error_text(error).c_str());
        close();
        return -1;
    }

    /**
     * @brief 把 FFmpeg 错误码转为文本。
     *
     * @param[in] error FFmpeg 错误码。
     *
     * @return 错误说明字符串。
     */
    static std::string error_text(int error) {
        char text[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(error, text, sizeof(text));
        return text;
    }

    /**
     * @brief 关闭网络 IO 并释放输出上下文。
     *
     * @param[in] write_trailer 关闭时是否写入封装尾部。
     */
    void close(bool write_trailer = true) {
        /* close() 可重复调用，供正常析构、打开失败和运行期断线共同使用。 */
        if (!context_)
            return;
        // RTSP 的 write_trailer 还负责关闭内部 RTP/RTSP socket，失败路径也必须调用。
        if (ready_ && (write_trailer || config_.format == "rtsp")) {
            if (write_trailer) arm_deadline();
            av_write_trailer(context_);
        }
        if (!(context_->oformat->flags & AVFMT_NOFILE) && context_->pb)
            avio_closep(&context_->pb);
        avformat_free_context(context_);
        context_ = nullptr;
        video_ = nullptr;
        audio_ = nullptr;
        ready_ = false;
    }

    EndpointConfig config_;
    NetworkRoute *network_; // PublisherSet 拥有，销毁顺序晚于所有 Output。
    const publishing::Queue *queue_;
    unsigned queue_epoch_ = 0;
    int timeout_ms_;
    unsigned network_generation_ = 0;
    std::chrono::steady_clock::time_point deadline_{};
    AVFormatContext *context_ = nullptr;
    AVStream *video_ = nullptr;
    AVStream *audio_ = nullptr;
    uint64_t origin_us_ = 0;
    std::string codec_extra_;
    bool ready_ = false;
    unsigned incomplete_key_logs_ = 0;
    unsigned video_debug_packets_ = 0;
    unsigned audio_debug_packets_ = 0;
    std::chrono::steady_clock::time_point retry_after_{};
};

/** @brief 一个 URL 一个发送线程；队列锁不跨越任何 FFmpeg 网络调用。 */
class AsyncOutput final {
public:
    /** @brief 创建隔离端点。 @param config 输出配置。 @param network 共享链路状态。 */
    AsyncOutput(EndpointConfig config, NetworkRoute *network)
        : name_(config.name), output_(std::move(config), network, &queue_),
          worker_(&AsyncOutput::run, this) {}
    /** @brief 先取消 IO、停止队列，再等待线程退出；之后才释放 FFmpeg。 */
    ~AsyncOutput() { queue_.stop(); if (worker_.joinable()) worker_.join(); }
    /** @brief 仅入队，不做网络 IO。 @param packet 自有编码包。 @return 0 入队，-1 丢弃。 */
    int enqueue(const publishing::Packet &packet) noexcept {
        try { return queue_.push(packet) ? 0 : -1; }
        catch (const std::exception &) { queue_.reset(); return -1; }
    }
    /** @brief 数据无法复制时丢弃该输出当前 GOP。 @return 无返回值。 */
    void discard() { queue_.reset(); }
private:
    /** @brief 独占访问本端点 FFmpeg；失败与丢包只影响本队列。 @return 无返回值。 */
    void run() {
        prctl(PR_SET_NAME, name_.c_str(), 0, 0, 0);
        publishing::Packet packet;
        unsigned epoch = 0;
        uint64_t last_drops = 0;
        auto last_log = std::chrono::steady_clock::time_point{};
        while (queue_.pop(packet, epoch)) {
            if (g_stopping || queue_.cancelled(epoch)) continue;
            try {
                const auto now = std::chrono::steady_clock::now();
                const uint64_t drops = queue_.drops();
                if (drops != last_drops && now - last_log >= std::chrono::seconds(5)) {
                    LOG_WARN("%s isolated queue: dropped=%llu; resumes on key frame", name_.c_str(),
                             static_cast<unsigned long long>(drops));
                    last_drops = drops; last_log = now;
                }
                output_.prepare(epoch, packet.extra);
                const int ret = packet.video
                    ? output_.write_video(packet.data->data(), packet.data->size(), packet.pts, packet.key)
                    : output_.write_audio(packet.data->data(), packet.data->size(), packet.pts);
                if (ret < 0) queue_.reset();
            } catch (const std::exception &error) {
                LOG_ERROR("%s isolated worker error: %s", name_.c_str(), error.what());
                queue_.reset();
            }
            packet = publishing::Packet(); // 空闲等待期间不保留上一帧引用。
        }
    }
    std::string name_;
    publishing::Queue queue_; // 必须比 Output 活得更久，供中断回调读取。
    Output output_;
    std::thread worker_;
};

class PublisherSet final {
public:
    /**
     * @brief 初始化网络层并创建启用的端点。
     */
    PublisherSet() {
        /*
         * RTSP 与 RTMP 使用不同 URL/path，因此是四个独立 publisher：
         * main RTSP/RTMP 含音频，AI RTSP/RTMP 仅视频。
         */
        av_log_set_callback(ffmpeg_log);
        setup_route();
        if (avformat_network_init() < 0) throw std::runtime_error("FFmpeg network init failed");
        try {
            add("rtsp-main", "stream:enable_rtsp", "stream:rtsp_main_url", "rtsp", 0, true);
            add("rtmp-main", "stream:enable_rtmp", "stream:rtmp_main_url", "flv", 0, true);
            add("rtsp-ai", "stream:enable_rtsp", "stream:rtsp_ai_url", "rtsp", 1, false);
            add("rtmp-ai", "stream:enable_rtmp", "stream:rtmp_ai_url", "flv", 1, false);
        } catch (...) {
            outputs_.clear(); // 局部端点先停线程，之后才释放网络全局资源。
            avformat_network_deinit();
            throw;
        }
    }
    /**
     * @brief 销毁端点并反初始化网络层。
     */
    ~PublisherSet() {
        outputs_.clear();
        avformat_network_deinit();
    }
    /**
     * @brief 把视频扇出到同通道的所有端点。
     *
     * @param[in] id 媒体通道编号。
     * @param[in] data 待处理数据的首地址。
     * @param[in] size 待处理数据的字节数。
     * @param[in] pts Rockchip 硬件产生的微秒时间戳。
     * @param[in] key 是否为视频关键帧。
     *
     * @return 端点结果的按位或，0 表示均成功。
     */
    int video(int id, const void *data, size_t size, uint64_t pts, bool key) {
        if (g_stopping || id < 0 || id > 1 || !data || !size || size > 2 * 1024 * 1024) {
            if (id >= 0 && id <= 1) discard(id);
            return -1;
        }
        try {
            // 每路 VENC 仅一个调用线程；先缓存完整参数集，再进入可丢弃的队列。
            if (key) {
                const auto sets = parameter_sets(static_cast<const uint8_t *>(data), size,
                                                 video_codec(id) == AV_CODEC_ID_HEVC);
                if (sets.complete(video_codec(id) == AV_CODEC_ID_HEVC) &&
                    (!extra_[id] || *extra_[id] != sets.data))
                    extra_[id] = std::make_shared<const std::string>(sets.data);
            }
            publishing::Packet packet;
            const auto *bytes = static_cast<const uint8_t *>(data);
            packet.data = std::make_shared<const std::vector<uint8_t>>(bytes, bytes + size);
            packet.extra = extra_[id];
            packet.pts = pts; packet.video = true; packet.key = key;
            int ret = 0;
            for (auto &output : outputs_)
                if (output.first == id) ret |= output.second->enqueue(packet);
            return ret;
        } catch (const std::exception &) { discard(id); return -1; }
    }
    /**
     * @brief 把音频扇出到所有主码流端点。
     *
     * @param[in] data 待处理数据的首地址。
     * @param[in] size 待处理数据的字节数。
     * @param[in] pts Rockchip 硬件产生的微秒时间戳。
     *
     * @return 端点结果的按位或，0 表示均成功。
     */
    int audio(const void *data, size_t size, uint64_t pts) {
        if (g_stopping || !data || !size || size > 2 * 1024 * 1024) return -1;
        try {
            publishing::Packet packet;
            const auto *bytes = static_cast<const uint8_t *>(data);
            packet.data = std::make_shared<const std::vector<uint8_t>>(bytes, bytes + size);
            packet.pts = pts;
            int ret = 0;
            for (auto &output : outputs_)
                if (output.first == 0) ret |= output.second->enqueue(packet);
            return ret;
        } catch (const std::exception &) { discard(0); return -1; }
    }

private:
    /** @brief 丢弃指定媒体通道的输出 GOP。 @param id 媒体通道号。 @return 无返回值。 */
    void discard(int id) {
        for (auto &output : outputs_) if (output.first == id) output.second->discard();
    }
    /**
     * @brief 从启用的 URL 提取唯一接收端，建立可选网络切换模块。
     * @throws std::runtime_error 接收地址不一致或路由初始化失败。
     */
    void setup_route() {
        if (!rk_param_get_int("network:enable_failover", 0)) return;
        const char *keys[] = {"stream:rtsp_main_url", "stream:rtsp_ai_url",
                              "stream:rtmp_main_url", "stream:rtmp_ai_url"};
        std::string host;
        std::vector<int> probe_ports;
        for (int i = 0; i < 4; ++i) {
            if (!rk_param_get_int(i < 2 ? "stream:enable_rtsp" : "stream:enable_rtmp", 0))
                continue;
            char current[256]{};
            int port = -1;
            av_url_split(nullptr, 0, nullptr, 0, current, sizeof(current), &port,
                         nullptr, 0, rk_param_get_string(keys[i], ""));
            if (host.empty()) {
                host = current;
            } else if (host != current) {
                throw std::runtime_error("failover requires all URLs to use the same IPv4 receiver");
            }
            const int probe_port = port > 0 ? port : (i < 2 ? 554 : 1935);
            if (std::find(probe_ports.begin(), probe_ports.end(), probe_port) == probe_ports.end())
                probe_ports.push_back(probe_port);
        }
        network_.reset(new NetworkRoute(host, probe_ports));
    }

    /**
     * @brief 根据 INI 开关和 URL 添加端点。
     *
     * @param[in] name 名称或配置字段名。
     * @param[in] enable_key 控制端点是否启用的 INI 键。
     * @param[in] url_key 保存推流地址的 INI 键。
     * @param[in] format FFmpeg 输出封装格式。
     * @param[in] id 媒体通道编号。
     * @param[in] audio 端点是否包含音频轨道。
     */
    void add(const char *name, const char *enable_key, const char *url_key,
             const char *format, int id, bool audio) {
        if (!rk_param_get_int(enable_key, 0))
            return;
        const char *url = rk_param_get_string(url_key, "");
        if (url && url[0])
            outputs_.push_back({id, std::unique_ptr<AsyncOutput>(
                new AsyncOutput(EndpointConfig{name, url, format, id, audio}, network_.get()))});
    }
    std::unique_ptr<NetworkRoute> network_;
    std::vector<std::pair<int, std::unique_ptr<AsyncOutput>>> outputs_;
    std::array<std::shared_ptr<const std::string>, 2> extra_;

};

std::unique_ptr<PublisherSet> g_publishers;
/* init/deinit 是控制面操作；采集只入队，运行期 FFmpeg 由独立发送线程持有。 */
std::mutex g_publishers_lock;

} // namespace

/**
 * @brief 校验配置并创建全局推流集合。
 *
 * @return 0 表示成功，-1 表示失败。
 */
extern "C" int ffmpeg_publisher_init(void) {
    std::lock_guard<std::mutex> guard(g_publishers_lock);
    if (g_publishers)
        return 0;
    if (!publisher_self_check()) {
        LOG_ERROR("internal Annex-B parameter-set parser self-check failed");
        return -1;
    }
    /* 在创建线程之前拒绝不支持的编码和不合法的 NV12 尺寸。 */
    for (int id = 0; id < 2; ++id) {
        const char *codec = video_text(id, "output_data_type", "H.264");
        if (strcmp(codec, "H.264") && strcmp(codec, "H.265")) {
            LOG_ERROR("video.%d:output_data_type must be H.264 or H.265", id);
            return -1;
        }
        if ((video_value(id, "width", 0) & 1) || (video_value(id, "height", 0) & 1)) {
            LOG_ERROR("video.%d width and height must be even for NV12", id);
            return -1;
        }
    }
    const bool rtsp = rk_param_get_int("stream:enable_rtsp", 0) != 0;
    const bool rtmp = rk_param_get_int("stream:enable_rtmp", 0) != 0;
    if (!rtsp && !rtmp) {
        LOG_ERROR("at least one of stream:enable_rtsp or stream:enable_rtmp must be enabled");
        return -1;
    }
    /* 启用某协议时，主流和 AI 流两个发布 URL 都必须完整配置。 */
    const char *required_urls[] = {
        "stream:rtsp_main_url", "stream:rtsp_ai_url",
        "stream:rtmp_main_url", "stream:rtmp_ai_url"};
    const int first = rtsp ? 0 : 2;
    const int last = rtmp ? 4 : 2;
    for (int i = first; i < last; ++i) {
        if ((i < 2 && !rtsp) || (i >= 2 && !rtmp))
            continue;
        const char *url = rk_param_get_string(required_urls[i], "");
        if (!url || !url[0]) {
            LOG_ERROR("%s is required", required_urls[i]);
            return -1;
        }
    }
    /* FFmpeg 4.x 的传统 FLV muxer 不认识 HEVC，显式失败比推送坏流更容易排查。 */
    if (rk_param_get_int("stream:enable_rtmp", 0) &&
        (video_codec(0) == AV_CODEC_ID_HEVC || video_codec(1) == AV_CODEC_ID_HEVC)) {
        LOG_ERROR("FFmpeg 4.x FLV/RTMP does not support H.265; select H.264 or disable RTMP");
        return -1;
    }
    g_stopping = false;
    try { g_publishers.reset(new PublisherSet()); }
    catch (const std::exception &error) {
        LOG_ERROR("publisher init failed: %s", error.what());
        return -1;
    }
    return 0;
}

/** @brief 通知所有 FFmpeg IO 取消；在 join 音视频线程之前调用。 */
extern "C" void ffmpeg_publisher_interrupt(void) { g_stopping = true; }

/** @brief 销毁全局推流集合；必须在采集线程退出后调用。 */
extern "C" void ffmpeg_publisher_deinit(void) {
    std::lock_guard<std::mutex> guard(g_publishers_lock);
    g_publishers.reset();
}

/**
 * @brief 把一帧视频转发给全局推流集合。
 *
 * @param[in] id 媒体通道编号。
 * @param[in] data 待处理数据的首地址。
 * @param[in] size 待处理数据的字节数。
 * @param[in] pts Rockchip 硬件产生的微秒时间戳。
 * @param[in] key 是否为视频关键帧。
 *
 * @return 集合处理结果；未初始化时返回 -1。
 */
extern "C" int ffmpeg_publisher_write_video(int id, const void *data, size_t size,
                                              uint64_t pts, int key) {
    return g_publishers ? g_publishers->video(id, data, size, pts, key != 0) : -1;
}

/**
 * @brief 把音频转发给全局推流集合。
 *
 * @param[in] data 待处理数据的首地址。
 * @param[in] size 待处理数据的字节数。
 * @param[in] pts Rockchip 硬件产生的微秒时间戳。
 *
 * @return 集合处理结果；未初始化时返回 -1。
 */
extern "C" int ffmpeg_publisher_write_audio(const void *data, size_t size, uint64_t pts) {
    return g_publishers ? g_publishers->audio(data, size, pts) : -1;
}
