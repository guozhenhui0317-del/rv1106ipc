#include "ffmpeg_publisher.h"

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

/*
 * 从 Rockchip VENC 输出的 Annex-B 码流中提取解码参数集。
 *
 * VENC 数据由 00 00 01 或 00 00 00 01 起始码分隔。FFmpeg muxer 在写
 * RTSP SDP/FLV header 前需要 H.264 SPS+PPS 或 H.265 VPS+SPS+PPS，因此
 * 首个关键帧到达时扫描这些 NAL，并保持原始 Annex-B 格式存入 extradata。
 * 普通 slice/SEI 不属于全局参数，故不复制，避免 header 随帧内容膨胀。
 */
std::string parameter_sets(const uint8_t *data, size_t size, AVCodecID codec) {
    std::string out;
    size_t pos = 0;
    while (pos + 4 < size) {
        size_t start = size;
        size_t prefix = 0;
        for (size_t i = pos; i + 3 < size; ++i) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
                start = i; prefix = 3; break;
            }
            if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 &&
                data[i + 2] == 0 && data[i + 3] == 1) {
                start = i; prefix = 4; break;
            }
        }
        if (start == size || start + prefix >= size)
            break;
        size_t end = size;
        for (size_t i = start + prefix; i + 3 < size; ++i) {
            if (data[i] == 0 && data[i + 1] == 0 &&
                (data[i + 2] == 1 || (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1))) {
                end = i; break;
            }
        }
        /* H.264 NAL 类型位于低 5 位；H.265 位于第一个字节的 bit[6:1]。 */
        const uint8_t type = codec == AV_CODEC_ID_H264
                                 ? data[start + prefix] & 0x1f
                                 : (data[start + prefix] >> 1) & 0x3f;
        const bool wanted = codec == AV_CODEC_ID_H264
                                ? (type == 7 || type == 8)
                                : (type == 32 || type == 33 || type == 34);
        if (wanted)
            out.append(reinterpret_cast<const char *>(data + start), end - start);
        pos = end;
    }
    return out;
}

const char *video_text(int id, const char *field, const char *fallback) {
    /* 统一拼接 video.<通道>:<字段>，避免各调用点散落硬编码键名。 */
    char key[64];
    snprintf(key, sizeof(key), "video.%d:%s", id, field);
    return rk_param_get_string(key, fallback);
}

AVCodecID video_codec(int id) {
    const char *name = video_text(id, "output_data_type", "H.264");
    return !strcmp(name, "H.265") ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
}

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
    const std::string extra = parameter_sets(h264, sizeof(h264), AV_CODEC_ID_H264);
    return extra.size() == 16 && !memcmp(extra.data(), h264, 16);
}

int video_value(int id, const char *field, int fallback) {
    char key[64];
    snprintf(key, sizeof(key), "video.%d:%s", id, field);
    return rk_param_get_int(key, fallback);
}

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
    explicit Output(EndpointConfig config) : config_(std::move(config)) {}
    ~Output() { close(); }

    int write_video(const void *data, size_t size, uint64_t pts_us, bool key) {
        /*
         * 同一 Output 会分别收到视频线程和音频线程调用，必须串行访问
         * AVFormatContext。未连接或断线后只在关键帧上打开，保证接收端从
         * 完整 GOP 开始解码；3 秒退避避免服务器离线时高速重复连接刷日志。
         */
        std::lock_guard<std::mutex> guard(lock_);
        if (!ready_) {
            if (!key || std::chrono::steady_clock::now() < retry_after_)
                return 0;
            if (open(static_cast<const uint8_t *>(data), size, pts_us) != 0) {
                retry_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                return -1;
            }
        }
        return write_packet(video_, data, size, pts_us, key,
                            video_value(config_.stream_id, "dst_frame_rate_den", 1));
    }

    int write_audio(const void *data, size_t size, uint64_t pts_us) {
        std::lock_guard<std::mutex> guard(lock_);
        /*
         * 视频关键帧定义整个端点的时间原点。它到达以前不能写音频，否则
         * muxer 没有 header；早于原点的音频也必须丢弃以防无符号减法下溢。
         */
        if (!ready_ || !audio_ || pts_us < origin_us_)
            return 0;
        return write_packet(audio_, data, size, pts_us, false,
                            static_cast<int>(size) / std::max(1, audio_->codecpar->channels));
    }

private:
    int open(const uint8_t *first_key_frame, size_t size, uint64_t pts_us) {
        /* 每次重连都从全新的 context 开始，旧 socket/stream 状态不能复用。 */
        close();
        int ret = avformat_alloc_output_context2(&context_, nullptr, config_.format.c_str(),
                                                 config_.url.c_str());
        if (ret < 0 || !context_) {
            LOG_ERROR("%s: cannot allocate output context (%d)", config_.name.c_str(), ret);
            return -1;
        }

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

        /* muxer header 必须先于媒体包发送，所以只能从首个关键帧取得参数集。 */
        const std::string extra = parameter_sets(first_key_frame, size, vp->codec_id);
        if (extra.empty())
            return fail("key frame has no SPS/PPS (or VPS)", AVERROR_INVALIDDATA);
        vp->extradata = static_cast<uint8_t *>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!vp->extradata)
            return fail("cannot allocate codec extradata", AVERROR(ENOMEM));
        memcpy(vp->extradata, extra.data(), extra.size());
        vp->extradata_size = static_cast<int>(extra.size());

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

        /* RTSP 默认强制 TCP，适合网线直连；超时值同时用于连接和后续写操作。 */
        AVDictionary *options = nullptr;
        if (config_.format == "rtsp")
            av_dict_set(&options, "rtsp_transport",
                        rk_param_get_string("stream:rtsp_transport", "tcp"), 0);
        const int timeout_ms = rk_param_get_int("stream:io_timeout_ms", 5000);
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
        return 0;
    }

    int write_packet(AVStream *stream, const void *data, size_t size, uint64_t pts_us,
                     bool key, int duration_hint) {
        /*
         * av_new_packet 分配带 AV_INPUT_BUFFER_PADDING_SIZE 的 FFmpeg 自有缓冲区。
         * 必须复制，因为 RK_MPI_*_ReleaseStream 在本函数返回后立即回收 MB 内存，
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
        /* interleaved 接口按时间戳交织主流音视频；AI 流只有视频，行为相同。 */
        ret = av_interleaved_write_frame(context_, &packet);
        av_packet_unref(&packet);
        if (ret < 0) {
            LOG_ERROR("%s write failed: %s", config_.name.c_str(), error_text(ret).c_str());
            close();
            retry_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            return -1;
        }
        return 0;
    }

    int fail(const char *what, int error) {
        LOG_ERROR("%s %s: %s", config_.name.c_str(), what, error_text(error).c_str());
        close();
        return -1;
    }

    static std::string error_text(int error) {
        char text[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(error, text, sizeof(text));
        return text;
    }

    void close() {
        /* close() 可重复调用，供正常析构、打开失败和运行期断线共同使用。 */
        if (!context_)
            return;
        if (ready_)
            av_write_trailer(context_);
        if (!(context_->oformat->flags & AVFMT_NOFILE) && context_->pb)
            avio_closep(&context_->pb);
        avformat_free_context(context_);
        context_ = nullptr;
        video_ = nullptr;
        audio_ = nullptr;
        ready_ = false;
    }

    EndpointConfig config_;
    AVFormatContext *context_ = nullptr;
    AVStream *video_ = nullptr;
    AVStream *audio_ = nullptr;
    uint64_t origin_us_ = 0;
    bool ready_ = false;
    std::mutex lock_;
    std::chrono::steady_clock::time_point retry_after_{};
};

class PublisherSet final {
public:
    PublisherSet() {
        /*
         * RTSP 与 RTMP 使用不同 URL/path，因此是四个独立 publisher：
         * main RTSP/RTMP 含音频，AI RTSP/RTMP 仅视频。
         */
        av_log_set_callback(ffmpeg_log);
        avformat_network_init();
        add("rtsp-main", "stream:enable_rtsp", "stream:rtsp_main_url", "rtsp", 0, true);
        add("rtmp-main", "stream:enable_rtmp", "stream:rtmp_main_url", "flv", 0, true);
        add("rtsp-ai", "stream:enable_rtsp", "stream:rtsp_ai_url", "rtsp", 1, false);
        add("rtmp-ai", "stream:enable_rtmp", "stream:rtmp_ai_url", "flv", 1, false);
    }
    ~PublisherSet() {
        outputs_.clear();
        avformat_network_deinit();
    }
    int video(int id, const void *data, size_t size, uint64_t pts, bool key) {
        /* 一次 VENC 取流扇出给该 stream_id 的所有已启用协议端点。 */
        int ret = 0;
        for (auto &output : outputs_)
            if (output.first == id)
                ret |= output.second->write_video(data, size, pts, key);
        return ret;
    }
    int audio(const void *data, size_t size, uint64_t pts) {
        /* 只有 stream_id=0 的主码流端点接收音频。 */
        int ret = 0;
        for (auto &output : outputs_)
            if (output.first == 0)
                ret |= output.second->write_audio(data, size, pts);
        return ret;
    }

private:
    void add(const char *name, const char *enable_key, const char *url_key,
             const char *format, int id, bool audio) {
        if (!rk_param_get_int(enable_key, 0))
            return;
        const char *url = rk_param_get_string(url_key, "");
        if (url && url[0])
            outputs_.push_back({id, std::unique_ptr<Output>(
                new Output(EndpointConfig{name, url, format, id, audio}))});
    }
    std::vector<std::pair<int, std::unique_ptr<Output>>> outputs_;
};

std::unique_ptr<PublisherSet> g_publishers;
/* init/deinit 是控制面操作；运行期包写入由各 Output 自己的 mutex 保护。 */
std::mutex g_publishers_lock;

} // namespace

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
    g_publishers.reset(new PublisherSet());
    return 0;
}

extern "C" void ffmpeg_publisher_deinit(void) {
    std::lock_guard<std::mutex> guard(g_publishers_lock);
    g_publishers.reset();
}

extern "C" int ffmpeg_publisher_write_video(int id, const void *data, size_t size,
                                              uint64_t pts, int key) {
    return g_publishers ? g_publishers->video(id, data, size, pts, key != 0) : -1;
}

extern "C" int ffmpeg_publisher_write_audio(const void *data, size_t size, uint64_t pts) {
    return g_publishers ? g_publishers->audio(data, size, pts) : -1;
}
