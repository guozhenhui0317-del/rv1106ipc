#define LOG_TAG "media"

#include "media_channels.hpp"
#include "media_support.hpp"
#include "fps_overlay.hpp"
#include "media_health.hpp"

extern "C" {
#include "param.h"
#include "rockiva.h"
#include "rk_comm_rgn.h"
#include "rk_comm_venc.h"
#include "rk_comm_vi.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_rgn.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
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
constexpr int kViDev = 0;
constexpr int kViPipe = 0;
constexpr int kMain = 0;
constexpr int kAi = 1;
constexpr int kOverlay = 7;

/**
 * @brief 读取 video.N 整数配置。
 *
 * @param[in] id 媒体通道编号。
 * @param[in] name 名称或配置字段名。
 * @param[in] fallback 配置项不存在时使用的缺省值。
 *
 * @return 配置值或 fallback。
 */
int value(int id, const char *name, int fallback) {
    char key[64];
    snprintf(key, sizeof(key), "video.%d:%s", id, name);
    return rk_param_get_int(key, fallback);
}

/**
 * @brief 读取 video.N 字符串配置。
 *
 * @param[in] id 媒体通道编号。
 * @param[in] name 名称或配置字段名。
 * @param[in] fallback 配置项不存在时使用的缺省值。
 *
 * @return 配置字符串指针。
 */
const char *text(int id, const char *name, const char *fallback) {
    char key[64];
    snprintf(key, sizeof(key), "video.%d:%s", id, name);
    return rk_param_get_string(key, fallback);
}

/**
 * @brief 判断视频通道是否配置为 H.265。
 *
 * @param[in] id 媒体通道编号。
 *
 * @return true 表示 H.265，false 表示 H.264。
 */
bool h265(int id) { return strcmp(text(id, "output_data_type", "H.264"), "H.265") == 0; }

/**
 * @brief 判断 VENC 包是否为关键帧。
 *
 * @param[in] id 媒体通道编号。
 * @param[in] pack VENC 输出数据包。
 *
 * @return true 表示 IDR/I 帧，false 表示普通帧。
 */
bool key_frame(int id, const VENC_PACK_S &pack) {
    /* I slice 也可作为恢复点；publisher 会进一步确认其中包含参数集。 */
    if (h265(id))
        return pack.DataType.enH265EType == H265E_NALU_IDRSLICE ||
               pack.DataType.enH265EType == H265E_NALU_ISLICE;
    return pack.DataType.enH264EType == H264E_NALU_IDRSLICE ||
           pack.DataType.enH264EType == H264E_NALU_ISLICE;
}

} // namespace

class Video::Impl final {
public:
    /**
     * @brief 创建两路 VI/VENC、可选 RGN，并启动视频线程。
     *
     * @throws std::runtime_error 初始化或 SDK 操作失败。
     */
    explicit Impl(VideoSink sink) : sink_(sink) {
        if (!sink_)
            throw std::invalid_argument("video sink is required");
        try {
            /*
             * 建链顺序遵循 RK MPI：先启用 VI/VENC，再绑定，最后启动取流线程。
             * VI1 同时绑定 VENC1 并设置 depth=1，让 RockIVA 可以旁路 GetFrame；
             * 这是本工程只使用两个 VI 通道的关键。
             */
            npu_ = rk_param_get_int("video.source:enable_npu", 1) != 0;
            init_device();
            create_vi(kMain);
            create_vi(kAi);
            create_venc(kMain);
            create_venc(kAi);
            bind(kMain);
            bind(kAi);
            if (npu_)
                create_overlay();
            if (rk_param_get_int("osd:enable_fps", 1))
                fps_.reset(new FpsOverlay());
            running_ = true;
            main_thread_ = std::thread(&Impl::venc_loop, this, kMain);
            ai_stream_thread_ = std::thread(&Impl::venc_loop, this, kAi);
            if (npu_) {
                inference_thread_ = std::thread(&Impl::inference_loop, this);
                overlay_thread_ = std::thread(&Impl::overlay_loop, this);
            }
            LOG_INFO("video ready: VI0->VENC0 main, VI1->VENC1 AI stream");
        } catch (...) {
            stop();
            throw;
        }
    }

    /**
     * @brief 停止视频线程并释放视频资源。
     */
    ~Impl() { stop(); }

private:
    /**
     * @brief 配置并启用 VI 设备 0。
     *
     * @throws std::runtime_error 初始化或 SDK 操作失败。
     */
    void init_device() {
        /*
         * 官方 rkipc 或其他进程可能已配置 VI device。先查询状态，只补做缺失
         * 的 SetAttr/Enable；启用后把 device 0 绑定到 ISP 输出 pipe 0。
         */
        VI_DEV_ATTR_S attr;
        memset(&attr, 0, sizeof(attr));
        int ret = RK_MPI_VI_GetDevAttr(kViDev, &attr);
        if (ret == RK_ERR_VI_NOT_CONFIG)
            require_ok(RK_MPI_VI_SetDevAttr(kViDev, &attr), "RK_MPI_VI_SetDevAttr");
        ret = RK_MPI_VI_GetDevIsEnable(kViDev);
        if (ret != RK_SUCCESS) {
            require_ok(RK_MPI_VI_EnableDev(kViDev), "RK_MPI_VI_EnableDev");
            VI_DEV_BIND_PIPE_S binding;
            memset(&binding, 0, sizeof(binding));
            binding.u32Num = 1;
            binding.PipeId[0] = kViPipe;
            require_ok(RK_MPI_VI_SetDevBindPipe(kViDev, &binding), "RK_MPI_VI_SetDevBindPipe");
        }
        device_ = true;
    }

    /**
     * @brief 创建并启用指定 VI 通道。
     *
     * @param[in] id 媒体通道编号。
     *
     * @throws std::runtime_error 初始化或 SDK 操作失败。
     */
    void create_vi(int id) {
        /*
         * 两路 VI 都输出 NV12/DMABUF。主路 depth=0，仅由 bind 直送 VENC，
         * 可少占一份缓存；AI 路需要 CPU 侧 GetChnFrame，因此 depth 必须大于 0。
         */
        VI_CHN_ATTR_S attr;
        memset(&attr, 0, sizeof(attr));
        attr.stIspOpt.u32BufCount = value(id, "input_buffer_count", id == kAi && npu_ ? 3 : 2);
        /* DMABUF fd 可直接交给 RockIVA，避免把 NV12 图像复制到用户态。 */
        attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
        attr.stIspOpt.stMaxSize.u32Width = value(id, "max_width", id ? 640 : 1920);
        attr.stIspOpt.stMaxSize.u32Height = value(id, "max_height", id ? 360 : 1080);
        attr.stSize.u32Width = value(id, "width", id ? 640 : 1920);
        attr.stSize.u32Height = value(id, "height", id ? 360 : 1080);
        attr.enPixelFormat = RK_FMT_YUV420SP;
        attr.enCompressMode = COMPRESS_MODE_NONE;
        attr.u32Depth = id == kAi && npu_ ? 1 : 0;
        attr.stFrameRate.s32SrcFrameRate = rk_param_get_int("isp.0.adjustment:fps", 25);
        attr.stFrameRate.s32DstFrameRate = value(id, "dst_frame_rate_num", 25) /
                                           std::max(1, value(id, "dst_frame_rate_den", 1));
        require_ok(RK_MPI_VI_SetChnAttr(kViPipe, id, &attr), "RK_MPI_VI_SetChnAttr");
        require_ok(RK_MPI_VI_EnableChn(kViPipe, id), "RK_MPI_VI_EnableChn");
        vi_created_[id] = true;
    }

    /**
     * @brief 设置 VENC 帧率、GOP 和码率控制属性。
     *
     * @param[in,out] attr 待读取或修改的通道属性。
     * @param[in] id 媒体通道编号。
     * @param[in] use_h265 是否使用 H.265 编码参数。
     * @param[in] cbr 是否使用 CBR 码率控制。
     */
    void set_rate(VENC_CHN_ATTR_S &attr, int id, bool use_h265, bool cbr) {
        /*
         * RK SDK 为 H264/H265、CBR/VBR 提供四套不同结构体，字段意义相同但
         * 无公共基类，只在此处集中分支，create_venc 不再重复码率控制逻辑。
         * INI 码率单位为 kbps，Rockchip 结构同样使用 kbps，无需乘 1000。
         */
        const RK_U32 src_num = value(id, "src_frame_rate_num", 25);
        const RK_U32 src_den = value(id, "src_frame_rate_den", 1);
        const RK_U32 dst_num = value(id, "dst_frame_rate_num", 25);
        const RK_U32 dst_den = value(id, "dst_frame_rate_den", 1);
        const RK_U32 gop = value(id, "gop", 50);
        const RK_U32 max_rate = value(id, "max_rate", id ? 512 : 4096);
        if (!use_h265 && cbr) {
            auto &p = attr.stRcAttr.stH264Cbr;
            attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
            p.u32Gop = gop; p.u32BitRate = max_rate;
            p.u32SrcFrameRateNum = src_num; p.u32SrcFrameRateDen = src_den;
            p.fr32DstFrameRateNum = dst_num; p.fr32DstFrameRateDen = dst_den;
        } else if (!use_h265) {
            auto &p = attr.stRcAttr.stH264Vbr;
            attr.stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
            p.u32Gop = gop; p.u32BitRate = value(id, "mid_rate", max_rate / 2);
            p.u32MaxBitRate = max_rate; p.u32MinBitRate = value(id, "min_rate", 0);
            p.u32SrcFrameRateNum = src_num; p.u32SrcFrameRateDen = src_den;
            p.fr32DstFrameRateNum = dst_num; p.fr32DstFrameRateDen = dst_den;
        } else if (cbr) {
            auto &p = attr.stRcAttr.stH265Cbr;
            attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
            p.u32Gop = gop; p.u32BitRate = max_rate;
            p.u32SrcFrameRateNum = src_num; p.u32SrcFrameRateDen = src_den;
            p.fr32DstFrameRateNum = dst_num; p.fr32DstFrameRateDen = dst_den;
        } else {
            auto &p = attr.stRcAttr.stH265Vbr;
            attr.stRcAttr.enRcMode = VENC_RC_MODE_H265VBR;
            p.u32Gop = gop; p.u32BitRate = value(id, "mid_rate", max_rate / 2);
            p.u32MaxBitRate = max_rate; p.u32MinBitRate = value(id, "min_rate", 0);
            p.u32SrcFrameRateNum = src_num; p.u32SrcFrameRateDen = src_den;
            p.fr32DstFrameRateNum = dst_num; p.fr32DstFrameRateDen = dst_den;
        }
    }

    /**
     * @brief 创建、配置并启动指定 VENC 通道。
     *
     * @param[in] id 媒体通道编号。
     *
     * @throws std::runtime_error 初始化或 SDK 操作失败。
     */
    void create_venc(int id) {
        /* VENC 完成硬件编码；后续 FFmpeg 只封装，不再次编码。 */
        VENC_CHN_ATTR_S attr;
        memset(&attr, 0, sizeof(attr));
        const bool use_h265 = h265(id);
        attr.stVencAttr.enType = use_h265 ? RK_VIDEO_ID_HEVC : RK_VIDEO_ID_AVC;
        if (!use_h265) {
            /* Rockchip MPI 使用标准 profile_idc 数值：66/77/100。 */
            const char *profile = text(id, "h264_profile", "high");
            attr.stVencAttr.u32Profile = !strcmp(profile, "baseline") ? 66 :
                                         !strcmp(profile, "main") ? 77 : 100;
        }
        attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
        attr.stVencAttr.u32MaxPicWidth = value(id, "max_width", id ? 640 : 1920);
        attr.stVencAttr.u32MaxPicHeight = value(id, "max_height", id ? 360 : 1080);
        attr.stVencAttr.u32PicWidth = value(id, "width", id ? 640 : 1920);
        attr.stVencAttr.u32PicHeight = value(id, "height", id ? 360 : 1080);
        attr.stVencAttr.u32VirWidth = attr.stVencAttr.u32PicWidth;
        attr.stVencAttr.u32VirHeight = attr.stVencAttr.u32PicHeight;
        attr.stVencAttr.u32StreamBufCnt = value(id, "buffer_count", 4);
        attr.stVencAttr.u32BufSize = value(id, "buffer_size",
                                          attr.stVencAttr.u32PicWidth * attr.stVencAttr.u32PicHeight);
        set_rate(attr, id, use_h265, !strcmp(text(id, "rc_mode", "CBR"), "CBR"));
        /* SmartP 使用虚拟 IDR/长期参考帧降码率；默认 normalP 兼容性最好。 */
        if (!strcmp(text(id, "gop_mode", "normalP"), "smartP")) {
            attr.stGopAttr.enGopMode = VENC_GOPMODE_SMARTP;
            attr.stGopAttr.s32VirIdrLen = value(id, "smartp_viridrlen", 25);
            attr.stGopAttr.u32MaxLtrCount = 1;
        } else {
            attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
        }
        require_ok(RK_MPI_VENC_CreateChn(id, &attr), "RK_MPI_VENC_CreateChn");
        venc_created_[id] = true;
        apply_quality(id, use_h265);
        if (value(id, "enable_refer_buffer_share", 1)) {
            VENC_CHN_REF_BUF_SHARE_S share;
            memset(&share, 0, sizeof(share));
            share.bEnable = RK_TRUE;
            require_ok(RK_MPI_VENC_SetChnRefBufShareAttr(id, &share),
                       "RK_MPI_VENC_SetChnRefBufShareAttr");
        }
        VENC_RECV_PIC_PARAM_S receive;
        memset(&receive, 0, sizeof(receive));
        /* -1 表示持续编码，直到 stop() 显式停止接收。 */
        receive.s32RecvPicNum = -1;
        require_ok(RK_MPI_VENC_StartRecvFrame(id, &receive), "RK_MPI_VENC_StartRecvFrame");
    }

    /**
     * @brief 应用画质等级和 QP 范围。
     *
     * @param[in] id 媒体通道编号。
     * @param[in] use_h265 是否使用 H.265 编码参数。
     *
     * @throws std::runtime_error 初始化或 SDK 操作失败。
     */
    void apply_quality(int id, bool use_h265) {
        /*
         * 将官方 INI 的文字质量等级映射为最小 QP：QP 越小画质越高、码率越大。
         * I/P 帧上下限继续允许在 INI 中精调，以适配不同现场噪声和带宽。
         */
        const char *quality = text(id, "rc_quality", "high");
        const int min_qp = !strcmp(quality, "highest") ? 10 :
                           !strcmp(quality, "higher") ? 15 :
                           !strcmp(quality, "high") ? 20 :
                           !strcmp(quality, "medium") ? 25 :
                           !strcmp(quality, "low") ? 30 :
                           !strcmp(quality, "lower") ? 35 : 40;
        VENC_RC_PARAM_S rc;
        memset(&rc, 0, sizeof(rc));
        require_ok(RK_MPI_VENC_GetRcParam(id, &rc), "RK_MPI_VENC_GetRcParam");
        if (use_h265) {
            rc.stParamH265.u32MinQp = min_qp;
            rc.stParamH265.u32FrmMinIQp = value(id, "frame_min_i_qp", 26);
            rc.stParamH265.u32FrmMinQp = value(id, "frame_min_qp", 28);
            rc.stParamH265.u32FrmMaxIQp = value(id, "frame_max_i_qp", 51);
            rc.stParamH265.u32FrmMaxQp = value(id, "frame_max_qp", 51);
        } else {
            rc.stParamH264.u32MinQp = min_qp;
            rc.stParamH264.u32FrmMinIQp = value(id, "frame_min_i_qp", 26);
            rc.stParamH264.u32FrmMinQp = value(id, "frame_min_qp", 28);
            rc.stParamH264.u32FrmMaxIQp = value(id, "frame_max_i_qp", 51);
            rc.stParamH264.u32FrmMaxQp = value(id, "frame_max_qp", 51);
        }
        require_ok(RK_MPI_VENC_SetRcParam(id, &rc), "RK_MPI_VENC_SetRcParam");
    }

    /**
     * @brief 把同编号 VI 绑定到 VENC。
     *
     * @param[in] id 媒体通道编号。
     *
     * @throws std::runtime_error 初始化或 SDK 操作失败。
     */
    void bind(int id) {
        /* VI->VENC 由内核/媒体框架直接传递 MB，应用不搬运原始 1080p 图像。 */
        MPP_CHN_S src = {RK_ID_VI, kViDev, id};
        MPP_CHN_S dst = {RK_ID_VENC, 0, id};
        require_ok(RK_MPI_SYS_Bind(&src, &dst), "RK_MPI_SYS_Bind(VI,VENC)");
        bound_[id] = true;
    }

    /**
     * @brief 创建 2BPP 检测框并绑定到 VENC1。
     *
     * @throws std::runtime_error 初始化或 SDK 操作失败。
     */
    void create_overlay() {
        /*
         * RGN 绑定在 VENC1 输入侧，因此框会被编码进 AI 子码流，但不会污染
         * 主码流。2BPP 每像素仅两位，足够透明背景加两种框颜色，内存最小。
         */
        RGN_ATTR_S attr;
        memset(&attr, 0, sizeof(attr));
        attr.enType = OVERLAY_RGN;
        attr.unAttr.stOverlay.enPixelFmt = RK_FMT_2BPP;
        attr.unAttr.stOverlay.u32CanvasNum = 1;
        /* RGN canvas 宽高按硬件要求向上对齐到 16 像素。 */
        attr.unAttr.stOverlay.stSize.u32Width = (value(kAi, "width", 640) + 15) & ~15;
        attr.unAttr.stOverlay.stSize.u32Height = (value(kAi, "height", 360) + 15) & ~15;
        require_ok(RK_MPI_RGN_Create(kOverlay, &attr), "RK_MPI_RGN_Create");
        overlay_created_ = true;

        RGN_CHN_ATTR_S channel;
        memset(&channel, 0, sizeof(channel));
        channel.bShow = RK_TRUE;
        channel.enType = OVERLAY_RGN;
        /* LUT 索引 0 作为透明背景，非零像素通过前景 alpha 完全显示。 */
        channel.unChnAttr.stOverlayChn.u32BgAlpha = 0;
        channel.unChnAttr.stOverlayChn.u32FgAlpha = 255;
        channel.unChnAttr.stOverlayChn.u32Layer = kOverlay;
        channel.unChnAttr.stOverlayChn.u32ColorLUT[0] = 0xFF0000;
        channel.unChnAttr.stOverlayChn.u32ColorLUT[1] = 0x0000FF;
        MPP_CHN_S target = {RK_ID_VENC, 0, kAi};
        require_ok(RK_MPI_RGN_AttachToChn(kOverlay, &target, &channel),
                   "RK_MPI_RGN_AttachToChn");
        overlay_attached_ = true;
    }

    /**
     * @brief 持续取得 VENC 码流并交给 publisher。
     *
     * @param[in] id 媒体通道编号。
     */
    void venc_loop(int id) {
        /* 每个 VENC 独立阻塞取流，避免一路网络/编码抖动阻塞另一路。 */
        prctl(PR_SET_NAME, id == kMain ? "venc-main" : "venc-ai", 0, 0, 0);
        VENC_PACK_S pack;
        VENC_STREAM_S stream;
        memset(&pack, 0, sizeof(pack));
        memset(&stream, 0, sizeof(stream));
        stream.pstPack = &pack;
        unsigned debug_frames = 0;
        while (running_) {
            const int ret = RK_MPI_VENC_GetStream(id, &stream, 1000);
            if (ret != RK_SUCCESS)
                continue;
            /*
             * 本 SDK 与官方 RV1106 RKIPC Demo 均直接使用 Handle2VirAddr 返回值。
             * 返回的 MB 句柄已指向本次有效码流；u32Offset 是底层环形缓冲区位置，
             * 不能再次叠加，否则会越过本次映射的 u32Len 字节。
             */
            uint8_t *base = static_cast<uint8_t *>(RK_MPI_MB_Handle2VirAddr(pack.pMbBlk));
            if (base && pack.u32Len) {
                if (debug_frames++ < 6)
                    LOG_INFO("VENC%d frame: seq=%u len=%u offset=%u mb=%llu data_num=%u "
                             "frame_end=%d stream_end=%d type=%d pts=%llu",
                             id, stream.u32Seq, pack.u32Len, pack.u32Offset,
                             static_cast<unsigned long long>(RK_MPI_MB_GetSize(pack.pMbBlk)),
                             pack.u32DataNum, pack.bFrameEnd, pack.bStreamEnd,
                             h265(id) ? static_cast<int>(pack.DataType.enH265EType)
                                      : static_cast<int>(pack.DataType.enH264EType),
                             static_cast<unsigned long long>(pack.u64PTS));
                // 统计实际取出的编码帧，不使用 INI 标称帧率，也不统计 IVA 推理次数。
                MediaHealth::instance().record(id);
                if (fps_) fps_->record(id);
                sink_(id, base, pack.u32Len,
                                             pack.u64PTS, key_frame(id, pack));
            }
            /* 回调已复制数据到自有包，故此处立即归还硬件 buffer，不等待网络。 */
            RK_MPI_VENC_ReleaseStream(id, &stream);
        }
    }

    /**
     * @brief 从 VI1 取得 DMABUF 帧并送入 RockIVA。
     */
    void inference_loop() {
        /*
         * AI 推理频率通常低于编码帧率。用 steady_clock 限频不会受系统时间校准
         * 影响；若一次推理已经超过周期则不额外 sleep，避免延迟继续累积。
         */
        prctl(PR_SET_NAME, "rockiva-input", 0, 0, 0);
        const int fps = std::max(1, rk_param_get_int("video.source:npu_fps", 10)); // 确保帧率至少为1
        const auto period = std::chrono::milliseconds(1000 / fps); // 计算两次推理之间的周期
        uint32_t frame_id = 0;
        while (running_) {
            const auto begin = std::chrono::steady_clock::now();  // 记录本轮开始时间
            VIDEO_FRAME_INFO_S frame;
            memset(&frame, 0, sizeof(frame));
            if (RK_MPI_VI_GetChnFrame(kViPipe, kAi, &frame, 1000) == RK_SUCCESS) {
                /* 仅传 DMABUF fd 给 RockIVA，实现 VI1 -> NPU 的零拷贝输入。 */
                const int fd = RK_MPI_MB_Handle2Fd(frame.stVFrame.pMbBlk);
                const int iva_ret = rkipc_rockiva_write_nv12_frame_by_fd(frame.stVFrame.u32Width,
                                                      frame.stVFrame.u32Height,
                                                      frame_id++, fd);
                RK_MPI_VI_ReleaseChnFrame(kViPipe, kAi, &frame);
                if (iva_ret == 0) MediaHealth::instance().record(3);
            }
            const auto spent = std::chrono::steady_clock::now() - begin;
            if (spent < period)
                std::this_thread::sleep_for(period - spent); // 补足剩余时间
        }
    }

    /**
     * @brief 在 2BPP canvas 中绘制裁剪后的空心矩形。
     *
     * @param[in,out] buffer 待绘制的 2BPP 画布。
     * @param[in] stride 画布的虚拟宽度。
     * @param[in] height 画布高度。
     * @param[in] x 矩形左上角横坐标。
     * @param[in] y 矩形左上角纵坐标。
     * @param[in] width 矩形宽度。
     * @param[in] rect_height 矩形高度。
     * @param[in] color 2BPP 颜色填充值。
     */
    static void rectangle(uint8_t *buffer, int stride, int height,
                          int x, int y, int width, int rect_height, uint8_t color) {
        /*
         * RK_FMT_2BPP 中一个字节容纳 4 个像素，所以 x/宽度需按 4 对齐。
         * 当前只画 4 像素粗的空心矩形；坐标先裁剪，防止 RockIVA 边界框在
         * 图像边缘因四舍五入写出 canvas。
         */
        x = std::max(0, x & ~3);  // RK_FMT_2BPP 是水平方向 4 个像素共用 1 个字节，而垂直方向仍然是一行一行独立存储
        y = std::max(0, y);
        width = std::min((width + 3) & ~3, stride - x);
        rect_height = std::min(rect_height, height - y);
        if (width < 8 || rect_height < 8)
            return;
        const int bytes_per_row = stride / 4;
        const int left = x / 4;
        const int right = (x + width) / 4 - 1;
        for (int row = 0; row < rect_height; ++row) {
            uint8_t *line = buffer + (y + row) * bytes_per_row;
            if (row < 4 || row >= rect_height - 4)
                memset(line + left, color, right - left + 1);
            else {
                line[left] = color;
                line[right] = color;
            }
        }
    }

    /**
     * @brief 读取检测结果并周期性刷新 VENC1 Overlay。
     */
    void overlay_loop() {
        /*
         * RockIVA 回调和 RGN canvas 更新解耦：本线程取最新检测结果，以 25 Hz
         * 刷新。300 ms 未收到结果便清框，避免目标离开后旧框长期残留。
         */
        prctl(PR_SET_NAME, "rockiva-osd", 0, 0, 0);
        const int width = value(kAi, "width", 640);
        const int height = value(kAi, "height", 360);
        RockIvaBaResult latest;
        memset(&latest, 0, sizeof(latest));
        auto last = std::chrono::steady_clock::time_point{};
        while (running_) {
            RockIvaBaResult update;
            memset(&update, 0, sizeof(update));
            if (rkipc_rknn_object_get(&update) == 0) {
                latest = update;
                last = std::chrono::steady_clock::now();
            } else if (last != std::chrono::steady_clock::time_point{} &&
                       std::chrono::steady_clock::now() - last > std::chrono::milliseconds(300)) {
                latest.objNum = 0;
            }

            RGN_CANVAS_INFO_S canvas;
            memset(&canvas, 0, sizeof(canvas));
            if (RK_MPI_RGN_GetCanvasInfo(kOverlay, &canvas) == RK_SUCCESS) {
                auto *pixels = reinterpret_cast<uint8_t *>(canvas.u64VirAddr);
                memset(pixels, 0, canvas.u32VirWidth * canvas.u32VirHeight / 4);
                for (int i = 0; i < latest.objNum; ++i) {
                    const RockIvaBaObjectInfo &object = latest.triggerObjects[i];
                    /* RockIVA 坐标范围为 0..10000，这里换算为 AI 码流像素。 */
                    int x = width * object.objInfo.rect.topLeft.x / 10000;
                    int y = height * object.objInfo.rect.topLeft.y / 10000;
                    int w = width * (object.objInfo.rect.bottomRight.x -
                                     object.objInfo.rect.topLeft.x) / 10000;
                    int h = height * (object.objInfo.rect.bottomRight.y -
                                      object.objInfo.rect.topLeft.y) / 10000;
                    rectangle(pixels, canvas.u32VirWidth, canvas.u32VirHeight,
                              x, y, w, h, object.objInfo.type == ROCKIVA_OBJECT_TYPE_PERSON
                                              ? 0xff : 0xaa);
                }
                RK_MPI_RGN_UpdateCanvas(kOverlay);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }

    /**
     * @brief 停止视频线程并反序释放 RGN、VENC 和 VI。
     */
    void stop() noexcept {
        /*
         * 先置 false 并等待所有线程退出，保证没有线程再访问 MPI handle；随后
         * 按 Attach/Bind/Create 的严格反序释放。每个布尔标志只表示对应调用
         * 已成功，因此构造函数在任意中间步骤抛异常时也能安全调用 stop()。
         */
        running_ = false;
        LOG_INFO("shutdown: join inference_thread_ begin");
        if (inference_thread_.joinable()) inference_thread_.join();
        LOG_INFO("shutdown: join inference_thread_ done");
        LOG_INFO("shutdown: join overlay_thread_ begin");
        if (overlay_thread_.joinable()) overlay_thread_.join();
        LOG_INFO("shutdown: join overlay_thread_ done");
        LOG_INFO("shutdown: join main_thread_ begin");
        if (main_thread_.joinable()) main_thread_.join();
        LOG_INFO("shutdown: join main_thread_ done");
        LOG_INFO("shutdown: join ai_stream_thread_ begin");
        if (ai_stream_thread_.joinable()) ai_stream_thread_.join();
        LOG_INFO("shutdown: join ai_stream_thread_ done");
        fps_.reset(); // 取流线程停止后释放，且早于 VENC 销毁。
        if (overlay_attached_) {
            MPP_CHN_S target = {RK_ID_VENC, 0, kAi};
            media_detail::cleanup("RK_MPI_RGN_DetachFromChn(kOverlay, &target)", [&] { return RK_MPI_RGN_DetachFromChn(kOverlay, &target); });
            overlay_attached_ = false;
        }
        if (overlay_created_) {
            media_detail::cleanup("RK_MPI_RGN_Destroy(kOverlay)", [&] { return RK_MPI_RGN_Destroy(kOverlay); });
            overlay_created_ = false;
        }
        for (int id = kAi; id >= kMain; --id) {
            if (bound_[id]) {
                MPP_CHN_S src = {RK_ID_VI, kViDev, id};
                MPP_CHN_S dst = {RK_ID_VENC, 0, id};
                media_detail::cleanup("RK_MPI_SYS_UnBind(&src, &dst)", [&] { return RK_MPI_SYS_UnBind(&src, &dst); });
                bound_[id] = false;
            }
            if (venc_created_[id]) {
                media_detail::cleanup("RK_MPI_VENC_StopRecvFrame(id)", [&] { return RK_MPI_VENC_StopRecvFrame(id); });
                media_detail::cleanup("RK_MPI_VENC_DestroyChn(id)", [&] { return RK_MPI_VENC_DestroyChn(id); });
                venc_created_[id] = false;
            }
            if (vi_created_[id]) {
                media_detail::cleanup("RK_MPI_VI_DisableChn(kViPipe, id)", [&] { return RK_MPI_VI_DisableChn(kViPipe, id); });
                vi_created_[id] = false;
            }
        }
        if (device_) {
            media_detail::cleanup("RK_MPI_VI_DisableDev(kViDev)", [&] { return RK_MPI_VI_DisableDev(kViDev); });
            device_ = false;
        }
    }

    VideoSink sink_;
    std::unique_ptr<FpsOverlay> fps_;
    std::atomic<bool> running_{false};
    bool npu_ = false;
    bool device_ = false;
    bool vi_created_[2] = {false, false};
    bool venc_created_[2] = {false, false};
    bool bound_[2] = {false, false};
    bool overlay_created_ = false;
    bool overlay_attached_ = false;
    std::thread main_thread_, ai_stream_thread_, inference_thread_, overlay_thread_;
};


Video::Video(VideoSink sink) : impl_(new Impl(sink)) {}
Video::~Video() = default;

} // namespace media_detail
