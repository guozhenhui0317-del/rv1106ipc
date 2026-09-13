#ifndef RV1106_CUSTOM_FFMPEG_PUBLISHER_H
#define RV1106_CUSTOM_FFMPEG_PUBLISHER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 FFmpeg 网络层和端点配置。
 *
 * @return 0 表示成功，-1 表示失败。
 */
int ffmpeg_publisher_init(void);

/**
 * @brief 关闭端点并释放 FFmpeg 网络资源。
 */
void ffmpeg_publisher_deinit(void);

/** @brief 中断网络等待，不释放上下文；供停止采集线程前调用。 */
void ffmpeg_publisher_interrupt(void);

/**
 * @brief 复制并入队一帧 VENC Annex-B 视频；返回后调用者可归还 MPI 缓冲区。
 *
 * @param[in] stream_id 视频流编号，0 为主码流，1 为 AI 子码流。
 * @param[in] data 待处理数据的首地址。
 * @param[in] size 待处理数据的字节数。
 * @param[in] pts_us Rockchip 硬件产生的微秒时间戳。
 * @param[in] key_frame 非零表示视频关键帧。
 *
 * @return 0 表示入队；-1 表示至少一路丢弃或复制失败，不代表其他输出失败。
 */
int ffmpeg_publisher_write_video(int stream_id, const void *data, size_t size,
                                 uint64_t pts_us, int key_frame);

/**
 * @brief 复制并入队一帧 G711A 音频，不等待网络写入。
 *
 * @param[in] data 待处理数据的首地址。
 * @param[in] size 待处理数据的字节数。
 * @param[in] pts_us Rockchip 硬件产生的微秒时间戳。
 *
 * @return 0 表示入队；-1 表示至少一路丢弃或复制失败，不代表其他输出失败。
 */
int ffmpeg_publisher_write_audio(const void *data, size_t size, uint64_t pts_us);

#ifdef __cplusplus
}
#endif
#endif
