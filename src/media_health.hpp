#pragma once

#include <atomic>
#include <chrono>

/** @brief 采集进展检测；不把推流连接状态当作摄像头健康状态。 */
class MediaHealth final {
public:
    /** @brief 获取进程内共享检测器。 @return 唯一检测器引用。 */
    static MediaHealth &instance() { static MediaHealth health; return health; }
    /** @brief 读取单调时钟。 @return 启动以来的秒数，不受 NTP 校时影响。 */
    static long now() {
        return static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    /** @brief 初始化宽限时间；须在创建媒体线程前调用。
     * @param audio 是否检测 AENC0。
     * @param iva 是否检测 IVA 输入处理完成。
     * @param time 单调时钟秒数。
     * @return 无返回值。
     */
    void reset(bool audio, bool iva, long time = now()) {
        enabled_[0] = enabled_[1] = true;
        enabled_[2] = audio;
        enabled_[3] = iva;
        for (auto &stamp : last_) stamp.store(time);
    }
    /** @brief 记录一次有效帧进展。
     * @param channel 0/1 为 VENC，2 为 AENC，3 为 IVA。
     * @param time 单调时钟秒数。
     * @return 无返回值；非法通道忽略。
     */
    void record(unsigned channel, long time = now()) {
        if (channel < 4) last_[channel].store(time);
    }
    /** @brief 查找持续 60 秒无进展的启用通道。
     * @param time 单调时钟秒数。
     * @return 故障通道名；全部正常时返回 nullptr。
     */
    const char *stalled(long time = now()) const {
        static const char *names[] = {"VENC0", "VENC1", "AENC0", "IVA"};
        for (unsigned i = 0; i < 4; ++i)
            if (enabled_[i] && time - last_[i].load() >= 60) return names[i];
        return nullptr;
    }
private:
    bool enabled_[4] = {};
    std::atomic<long> last_[4]{};
};
