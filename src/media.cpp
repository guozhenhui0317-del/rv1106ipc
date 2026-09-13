#define LOG_TAG "media"

#include "media.hpp"
#include "media_channels.hpp"
#include "media_support.hpp"
#include "ffmpeg_publisher.h"

extern "C" {
#include "isp.h"
#include "param.h"
#include "rockiva.h"
#include <rk_mpi_sys.h>
}

using media_detail::require_ok;

class MediaRuntime::MediaPipeline final {
public:
    /**
     * @brief 初始化 ISP、可选 RockIVA、MPI、推流、视频和音频。
     *
     * @param[in] iq_dir RKAIQ IQ 文件目录。
     *
     * @throws std::runtime_error 初始化或 SDK 操作失败。
     */
    explicit MediaPipeline(const std::string &iq_dir) {
        try {
            /*
             * ISP 必须最先读取 SC3336 IQ；RockIVA 在推理线程前初始化；MPI 全局
             * 初始化必须早于所有 VI/VENC/AI/AENC/RGN 调用。publisher 在工作
             * 线程启动前校验全部 URL/编码配置，使配置错误不会留下半条链路。
             */
            if (rk_param_get_int("video.source:enable_aiq", 1)) {
                require_ok(rk_isp_init(0, const_cast<char *>(iq_dir.c_str())), "rk_isp_init");
                isp_ = true;
                if (rk_param_get_int("isp:init_form_ini", 1))
                    require_ok(rk_isp_set_from_ini(0), "rk_isp_set_from_ini");
            }
            if (rk_param_get_int("video.source:enable_npu", 1)) {
                require_ok(rkipc_rockiva_init(), "rkipc_rockiva_init");
                rockiva_ = true;
            }
            require_ok(RK_MPI_SYS_Init(), "RK_MPI_SYS_Init");
            mpi_ = true;
            if (ffmpeg_publisher_init() != 0)
                throw std::runtime_error("ffmpeg_publisher_init");
            publisher_ = true;
            video_.reset(new media_detail::Video(ffmpeg_publisher_write_video));
            if (rk_param_get_int("audio.0:enable", 1))
                audio_.reset(new media_detail::Audio(ffmpeg_publisher_write_audio));
        } catch (...) {
            stop();
            throw;
        }
    }
    /**
     * @brief 触发完整媒体运行时的反序清理。
     */
    ~MediaPipeline() { stop(); }

private:
    /**
     * @brief 按依赖反序停止完整媒体系统。
     */
    void stop() noexcept {
        // 与官方 RV1106 一致：视频、ISP、音频、MPI、IVA；先中断网络等待。
        if (publisher_) ffmpeg_publisher_interrupt();
        LOG_INFO("shutdown: video begin");
        video_.reset();
        LOG_INFO("shutdown: video done");
        if (isp_) { media_detail::cleanup("ISP", [] { return rk_isp_deinit(0); }); isp_ = false; }
        LOG_INFO("shutdown: audio begin");
        audio_.reset();
        LOG_INFO("shutdown: audio done");
        LOG_INFO("shutdown: publisher begin");
        if (publisher_) { ffmpeg_publisher_deinit(); publisher_ = false; }
        LOG_INFO("shutdown: publisher done");
        if (mpi_) { media_detail::cleanup("MPI", [] { return RK_MPI_SYS_Exit(); }); mpi_ = false; }
        if (rockiva_) { media_detail::cleanup("IVA", [] { return rkipc_rockiva_deinit(); }); rockiva_ = false; }
        LOG_INFO("shutdown: media cleanup finished; inspect preceding errors");
    }
    bool isp_ = false, rockiva_ = false, mpi_ = false, publisher_ = false;
    std::unique_ptr<media_detail::Video> video_;
    std::unique_ptr<media_detail::Audio> audio_;
};

/**
 * @brief 创建 MediaPipeline 并启动完整媒体链路。
 *
 * @param[in] iq_file_dir RKAIQ IQ 文件目录。
 *
 * @throws std::runtime_error 初始化或 SDK 操作失败。
 */
MediaRuntime::MediaRuntime(const std::string &iq_file_dir)
    : pipeline_(new MediaPipeline(iq_file_dir)) {}
/**
 * @brief 销毁 MediaPipeline 并释放全部媒体资源。
 */
MediaRuntime::~MediaRuntime() = default;
