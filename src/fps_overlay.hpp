#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

/** @brief 管理两路 VENC 的实际帧率统计和硬件文字叠加。 */
class FpsOverlay final {
public:
    /** @brief 在 VENC 创建后创建 RGN 0/1，启动每秒更新线程。
     * @throws std::runtime_error RGN 创建或绑定失败。
     */
    FpsOverlay();
    /** @brief 停止更新并释放 RGN，必须早于 VENC 销毁。 */
    ~FpsOverlay();
    FpsOverlay(const FpsOverlay &) = delete;
    FpsOverlay &operator=(const FpsOverlay &) = delete;
    /** @brief 记录一次成功取出的完整编码帧。
     * @param id VENC 通道 0 或 1；其他值忽略。
     * @return 无返回值。
     */
    void record(int id) { if (id >= 0 && id < 2) ++frames_[id]; }
private:
    /** @brief 为指定 VENC 创建并绑定 FPS 画布。 @param id 通道 0 或 1。 */
    void create(int id);
    /** @brief 每秒计算实际取帧数/单调时间间隔并刷新两路文字。 */
    void run();
    /** @brief 刷新 RGN。 @param id 通道编号。 @param fps 实际输出帧率。 */
    void update(int id, double fps);
    /** @brief 停止线程并反序释放资源，可重复调用。 */
    void stop() noexcept;
    std::atomic<unsigned> frames_[2];
    bool created_[2] = {false, false};
    bool attached_[2] = {false, false};
    std::mutex lock_;
    std::condition_variable wake_;
    bool stopped_ = false;
    std::thread worker_;
};
