#define LOG_TAG "media"

/**
 * @file audio.cpp
 * @brief 管理 AI0、可选 VQE、AENC0 和唯一音频取流线程。
 *
 * @details 数据流固定为：
 * @verbatim
 * 声卡 -> AI0 -> 可选 VQE -> AENC0 -> capture_thread_ -> AudioSink -> publisher
 * @endverbatim
 * AudioPipeline 独占硬件通道和线程；shutdown() 先 join 线程，再按绑定和
 * 创建的反序释放资源。
 */

#include "media/media_channels.hpp"
#include "media/media_support.hpp"
#include "media/media_health.hpp"

extern "C" {
#include "param.h"
#include "rk_comm_aio.h"
#include "rk_mpi_aenc.h"
#include "rk_mpi_ai.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <sys/prctl.h>

namespace media_detail {
namespace {
constexpr int kAenc = 0;

} // namespace

class Audio::AudioPipeline final {
public:
    /**
     * @brief 创建 AI0、可选 VQE、AENC0 并启动音频线程。
     * @param[in] sink G711A 接收函数；在音频取流线程中同步调用。
     * @throws std::runtime_error 初始化或 SDK 操作失败。
     */
    explicit AudioPipeline(AudioSink sink) : sink_(sink) {
        if (!sink_)
            throw std::invalid_argument("audio sink is required");
        try {
            validate_configuration();
            initialize_audio_input();
            create_encoder();
            bind_input_to_encoder();
            start_capture();
            LOG_INFO("audio ready: AI0->AENC0 G711A %d Hz %d channel(s)",
                     rk_param_get_int("audio.0:sample_rate", 8000),
                     rk_param_get_int("audio.0:channels", 1));
        } catch (...) {
            shutdown();
            throw;
        }
    }
    /**
     * @brief 停止音频线程并释放音频资源。
     */
    ~AudioPipeline() { shutdown(); }

private:
    /**
     * @brief 拒绝本硬件链路不支持的音频编码配置。
     * @return 无返回值。
     * @throws std::runtime_error 编码格式不是 G711A。
     */
    void validate_configuration() const {
        /* 本工程沿用官方 G711A 配置；FFmpeg 只复用编码结果，不做重采样。 */
        const char *codec = rk_param_get_string("audio.0:encode_type", "G711A");
        if (strcmp(codec, "G711A"))
            throw std::runtime_error("this target requires audio.0:encode_type=G711A");
    }

    /**
     * @brief 配置并启用 AI0、可选 VQE 和 AI0 通道 0。
     * @return 无返回值。
     * @throws std::runtime_error 任一 AI/VQE SDK 调用失败。
     */
    void initialize_audio_input() {
        AIO_ATTR_S ai;
        memset(&ai, 0, sizeof(ai));
        snprintf(reinterpret_cast<char *>(ai.u8CardName), sizeof(ai.u8CardName), "%s",
                 rk_param_get_string("audio.0:card_name", "hw:0,0"));
        /* 底层声卡为双通道，enSoundmode/TrackMode 决定发布单声道还是立体声。 */
        ai.soundCard.channels = 2;
        ai.soundCard.sampleRate = rk_param_get_int("audio.0:sample_rate", 8000);
        ai.soundCard.bitWidth = AUDIO_BIT_WIDTH_16;
        ai.enSamplerate = static_cast<AUDIO_SAMPLE_RATE_E>(ai.soundCard.sampleRate);
        ai.enBitwidth = AUDIO_BIT_WIDTH_16;
        ai.enSoundmode = rk_param_get_int("audio.0:channels", 1) == 1
                             ? AUDIO_SOUND_MODE_MONO : AUDIO_SOUND_MODE_STEREO;
        ai.u32FrmNum = 4;
        ai.u32PtNumPerFrm = rk_param_get_int("audio.0:frame_size", 1152);
        ai.u32ChnCnt = 2;
        require_ok(RK_MPI_AI_SetPubAttr(0, &ai), "RK_MPI_AI_SetPubAttr");
        require_ok(RK_MPI_AI_Enable(0), "RK_MPI_AI_Enable");
        ai_device_enabled_ = true;

        if (rk_param_get_int("audio.0:enable_vqe", 1)) {
            /* VQE 参数由 Rockchip JSON 加载，保持降噪/回声消除配置与 Demo 一致。 */
            AI_VQE_CONFIG_S vqe;
            memset(&vqe, 0, sizeof(vqe));
            vqe.enCfgMode = AIO_VQE_CONFIG_LOAD_FILE;
            snprintf(reinterpret_cast<char *>(vqe.aCfgFile), sizeof(vqe.aCfgFile), "%s",
                     rk_param_get_string("audio.0:vqe_cfg",
                                         "/oem/usr/share/vqefiles/config_aivqe.json"));
            vqe.s32WorkSampleRate = ai.soundCard.sampleRate;
            /* 官方 VQE 以 16 ms 为处理帧长。 */
            vqe.s32FrameSample = ai.soundCard.sampleRate * 16 / 1000;
            require_ok(RK_MPI_AI_SetVqeAttr(0, 0, 0, 0, &vqe), "RK_MPI_AI_SetVqeAttr");
            require_ok(RK_MPI_AI_EnableVqe(0, 0), "RK_MPI_AI_EnableVqe");
            vqe_enabled_ = true;
        }

        require_ok(RK_MPI_AI_EnableChn(0, 0), "RK_MPI_AI_EnableChn");
        ai_channel_enabled_ = true;
        RK_MPI_AI_SetVolume(0, rk_param_get_int("audio.0:volume", 50));
        if (rk_param_get_int("audio.0:channels", 1) == 1)
            RK_MPI_AI_SetTrackMode(0, AUDIO_TRACK_FRONT_LEFT);
    }

    /**
     * @brief 创建把 PCM 编码为 G711 A-law 的 AENC0。
     * @return 无返回值。
     * @throws std::runtime_error AENC0 创建失败。
     */
    void create_encoder() {
        AENC_CHN_ATTR_S encoder;
        memset(&encoder, 0, sizeof(encoder));
        encoder.enType = RK_AUDIO_ID_PCM_ALAW;
        encoder.stCodecAttr.enType = RK_AUDIO_ID_PCM_ALAW;
        encoder.stCodecAttr.u32Channels = rk_param_get_int("audio.0:channels", 1);
        encoder.stCodecAttr.u32SampleRate = rk_param_get_int("audio.0:sample_rate", 8000);
        encoder.stCodecAttr.enBitwidth = AUDIO_BIT_WIDTH_16;
        encoder.u32BufCount = 4;
        require_ok(RK_MPI_AENC_CreateChn(kAenc, &encoder), "RK_MPI_AENC_CreateChn");
        aenc_created_ = true;
    }

    /**
     * @brief 绑定 AI0 通道 0 到 AENC0，让 MPI 直接传递 PCM。
     * @return 无返回值。
     * @throws std::runtime_error MPI 绑定失败。
     */
    void bind_input_to_encoder() {
        MPP_CHN_S source = {RK_ID_AI, 0, 0};
        MPP_CHN_S target = {RK_ID_AENC, 0, kAenc};
        require_ok(RK_MPI_SYS_Bind(&source, &target), "RK_MPI_SYS_Bind(AI,AENC)");
        input_encoder_bound_ = true;
    }

    /**
     * @brief 在硬件链路完成后启动唯一音频取流线程。
     * @return 无返回值。
     * @throws std::system_error 线程创建失败。
     */
    void start_capture() {
        running_ = true;
        capture_thread_ = std::thread(&AudioPipeline::capture_encoded_audio, this);
    }

    /**
     * @brief 持续取得 AENC0 数据并交给主码流 publisher。
     * @return 无返回值；仅音频取流线程调用。
     */
    void capture_encoded_audio() {
        /* AENC PTS 原样传给 publisher，与 VENC PTS 共用 RK MPI 微秒时间轴。 */
        prctl(PR_SET_NAME, "aenc-main", 0, 0, 0);
        while (running_) {
            AUDIO_STREAM_S stream;
            memset(&stream, 0, sizeof(stream));
            if (RK_MPI_AENC_GetStream(kAenc, &stream, 1000) != RK_SUCCESS)
                continue;
            void *data = RK_MPI_MB_Handle2VirAddr(stream.pMbBlk);
            if (data && stream.u32Len) {
                MediaHealth::instance().record(2);
                sink_(data, stream.u32Len, stream.u64TimeStamp);
            }
            RK_MPI_AENC_ReleaseStream(kAenc, &stream);
        }
    }
    /**
     * @brief 停止音频线程并反序销毁 AI 到 AENC 链路。
     * @return 无返回值。
     */
    void shutdown() noexcept {
        /* 先停取流线程，再解除 AI->AENC 绑定，最后由下游向上游销毁。 */
        running_ = false;
        LOG_INFO("shutdown: join capture_thread_ begin");
        if (capture_thread_.joinable()) capture_thread_.join();
        LOG_INFO("shutdown: join capture_thread_ done");
        if (input_encoder_bound_) {
            MPP_CHN_S source = {RK_ID_AI, 0, 0};
            MPP_CHN_S target = {RK_ID_AENC, 0, kAenc};
            media_detail::cleanup("RK_MPI_SYS_UnBind(&source, &target)", [&] { return RK_MPI_SYS_UnBind(&source, &target); });
            input_encoder_bound_ = false;
        }
        if (aenc_created_) { media_detail::cleanup("RK_MPI_AENC_DestroyChn(kAenc)", [&] { return RK_MPI_AENC_DestroyChn(kAenc); }); aenc_created_ = false; }
        if (ai_channel_enabled_) { media_detail::cleanup("RK_MPI_AI_DisableChn(0, 0)", [&] { return RK_MPI_AI_DisableChn(0, 0); }); ai_channel_enabled_ = false; }
        if (vqe_enabled_) { media_detail::cleanup("RK_MPI_AI_DisableVqe(0, 0)", [&] { return RK_MPI_AI_DisableVqe(0, 0); }); vqe_enabled_ = false; }
        if (ai_device_enabled_) { media_detail::cleanup("RK_MPI_AI_Disable(0)", [&] { return RK_MPI_AI_Disable(0); }); ai_device_enabled_ = false; }
    }
    AudioSink sink_;
    std::atomic<bool> running_{false};
    bool ai_device_enabled_ = false, ai_channel_enabled_ = false, vqe_enabled_ = false;
    bool aenc_created_ = false, input_encoder_bound_ = false;
    std::thread capture_thread_;
};


Audio::Audio(AudioSink sink) : pipeline_(new AudioPipeline(sink)) {}
Audio::~Audio() = default;

} // namespace media_detail
