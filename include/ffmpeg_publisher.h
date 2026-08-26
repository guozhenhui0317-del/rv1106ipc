#ifndef RV1106_CUSTOM_FFMPEG_PUBLISHER_H
#define RV1106_CUSTOM_FFMPEG_PUBLISHER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 FFmpeg 网络层，并根据 INI 创建已启用的推流端点集合。
 *
 * 此函数只建立端点配置，不会立刻连接服务器。每个端点必须等到所属
 * VENC 通道产生第一个带 SPS/PPS（H.265 还需 VPS）的关键帧后才能写
 * muxer header，因此实际连接采用“首个关键帧时延迟打开”的方式。
 * 返回 0 表示配置有效，返回 -1 表示配置或内部自检失败。
 */
int ffmpeg_publisher_init(void);

/** 关闭全部输出、写 trailer，并释放 FFmpeg 网络资源。 */
void ffmpeg_publisher_deinit(void);

/**
 * 投递一帧 VENC Annex-B 码流。
 * @param stream_id 0 为 1080p 主码流，1 为带 AI 框的子码流。
 * @param pts_us    Rockchip VENC 给出的微秒时间戳；不得改成自增帧号。
 * @param key_frame 非零表示 IDR/I 帧，用于首次建连及断线重连。
 *
 * 函数会把数据复制到 AVPacket，因此返回后调用者可以立即释放 VENC 帧。
 */
int ffmpeg_publisher_write_video(int stream_id, const void *data, size_t size,
                                 uint64_t pts_us, int key_frame);

/**
 * 投递一帧 AENC G711A 数据。音频仅写入主码流端点；pts_us 与 VENC 使用
 * 同一个硬件微秒时基，publisher 会统一减去首个视频关键帧的时间原点。
 */
int ffmpeg_publisher_write_audio(const void *data, size_t size, uint64_t pts_us);

#ifdef __cplusplus
}
#endif
#endif
