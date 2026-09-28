#define LOG_TAG "media"

/**
 * @file media.cpp
 * @brief 装配整条媒体链路，并集中定义跨模块的启动和退出顺序。
 *
 * @details 所有权关系：
 * @verbatim
 * MediaRuntime
 *   `-- MediaPipeline
 *         |-- FFmpeg publisher（进程内全局实例）
 *         |-- Video（VI/VENC/RGN 与视频线程）
 *         `-- Audio（AI/VQE/AENC 与音频线程，可选）
 * @endverbatim
 * 构造按 ISP -> RockIVA -> MPI -> publisher -> Video -> Audio 执行；任一步
 * 抛出异常都会调用 shutdown() 回滚。正常退出同样走 shutdown()，先中断网络
 * 阻塞，再等待采集线程退出，最后释放底层全局资源。
 */

#include "media.hpp"
#include "media/media_channels.hpp"
#include "media/media_support.hpp"
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
            initialize_isp(iq_dir);
            initialize_rockiva();
            initialize_mpi();
            initialize_publisher();
            initialize_capture();
        } catch (...) {
            shutdown();
            throw;
        }
    }
    /**
     * @brief 触发完整媒体运行时的反序清理。
     */
    ~MediaPipeline() { shutdown(); }

private:
    /**
     * @brief 初始化 SC3336 对应的 RKAIQ/ISP。
     * @param[in] iq_dir RKAIQ IQ 文件目录。
     * @return 无返回值。
     * @throws std::runtime_error ISP 初始化或应用 INI 参数失败。
     */
    void initialize_isp(const std::string &iq_dir) {
        if (!rk_param_get_int("video.source:enable_aiq", 1))
            return;
        require_ok(rk_isp_init(0, const_cast<char *>(iq_dir.c_str())), "rk_isp_init");
        isp_ = true;
        if (rk_param_get_int("isp:init_form_ini", 1))
            require_ok(rk_isp_set_from_ini(0), "rk_isp_set_from_ini");
    }

    /**
     * @brief 在视频推理线程启动前初始化可选 RockIVA。
     * @return 无返回值。
     * @throws std::runtime_error RockIVA 初始化失败。
     */
    void initialize_rockiva() {
        if (!rk_param_get_int("video.source:enable_npu", 1))
            return;
        require_ok(rkipc_rockiva_init(), "rkipc_rockiva_init");
        rockiva_ = true;
    }

    /**
     * @brief 初始化 VI/VENC/AI/AENC/RGN 共用的 RK MPI 全局环境。
     * @return 无返回值。
     * @throws std::runtime_error RK MPI 初始化失败。
     */
    void initialize_mpi() {
        require_ok(RK_MPI_SYS_Init(), "RK_MPI_SYS_Init");
        mpi_ = true;
    }

    /**
     * @brief 校验推流配置并创建独立 RTSP/RTMP 端点。
     * @return 无返回值。
     * @throws std::runtime_error publisher 初始化失败。
     */
    void initialize_publisher() {
        if (ffmpeg_publisher_init() != 0)
            throw std::runtime_error("ffmpeg_publisher_init");
        publisher_ = true;
    }

    /**
     * @brief 最后创建音视频采集对象；其构造函数会启动采集线程。
     * @return 无返回值。
     * @throws std::runtime_error 音视频通道或线程初始化失败。
     */
    void initialize_capture() {
        video_.reset(new media_detail::Video(ffmpeg_publisher_write_video));
        if (rk_param_get_int("audio.0:enable", 1))
            audio_.reset(new media_detail::Audio(ffmpeg_publisher_write_audio));
    }

    /**
     * @brief 按依赖反序停止完整媒体系统。
     * @return 无返回值。
     */
    void shutdown() noexcept {
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
