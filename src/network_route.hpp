#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/** @brief 有线优先的切换判定；不操作网络，便于主机测试。 */
class LinkPolicy final {
public:
    /**
     * @brief 连续三次失败后切换，连续健康十秒后回切有线。
     * @param current 当前链路：-1 未选择，0 eth0，1 wlan0。
     * @param wired 有线到接收端是否可达。
     * @param wifi Wi-Fi 到接收端是否可达。
     * @param now_ms 单调时钟毫秒值。
     * @return 希望使用的链路编号；两路均故障时保留原选择。
     */
    int choose(int current, bool wired, bool wifi, int64_t now_ms) {
        wifi_failures_ = wifi ? 0 : (wifi_failures_ < 3 ? wifi_failures_ + 1 : 3);
        if (wired) {
            failures_ = 0;
            if (recovered_at_ < 0) recovered_at_ = now_ms;
        } else {
            if (failures_ < 3) ++failures_;
            recovered_at_ = -1;
        }
        if (current < 0) return wired ? 0 : (wifi ? 1 : -1);
        if (current == 0) return !wired && wifi && failures_ >= 3 ? 1 : 0;
        return wired && (wifi_failures_ >= 3 || now_ms - recovered_at_ >= 10000) ? 0 : 1;
    }
    /** @brief 已选链路是否尚未达到连续三次故障门限。
     * @param current -1 无链路，0 有线，1 Wi-Fi。@return 是否允许发布。
     */
    bool usable(int current) const {
        return current == 0 ? failures_ < 3 : current == 1 && wifi_failures_ < 3;
    }
private:
    int wifi_failures_ = 0;
    int failures_ = 0;
    int64_t recovered_at_ = -1;
};

/**
 * @brief 管理同网段 IPv4 接收端的临时 /32 路由，不配置网卡或默认网关。
 * @details 需要 root/CAP_NET_ADMIN。析构仅删除本实例创建的路由。
 */
class NetworkRoute final {
public:
    /** @brief 校验目标并启动检测线程。
     * @param host 接收端 IPv4 字符串；不接受域名或跨网关目标。
     * @param ports 启用的 RTSP/RTMP TCP 端口；任一响应即证明网络可达。
     * @throws std::runtime_error 配置冲突或网络初始化失败。
     */
    NetworkRoute(const std::string &host, const std::vector<int> &ports);
    /** @brief 停止检测并删除本实例添加的临时路由。 */
    ~NetworkRoute();
    NetworkRoute(const NetworkRoute &) = delete;
    NetworkRoute &operator=(const NetworkRoute &) = delete;
    /** @brief 返回路由代数，变化时旧推流连接必须重建。 @return 当前代数。 */
    unsigned generation() const { return generation_.load(); }
    /** @brief 查询路由已验证且链路未达到连续失败门限。 @return true 表示可以尝试推流。 */
    bool ready() const { return ready_.load(); }
private:
    /** @brief 绑定指定网卡及其 IPv4 地址探测接收端。
     * @param id 0 表示 eth0，1 表示 wlan0。
     * @param[out] local_address 本次探测使用的源 IPv4，未取得时为零。
     * @return TCP 建连成功或收到拒绝连接表示链路可达。
     */
    bool reachable(int id, uint32_t &local_address) const;
    /** @brief 添加或删除本实例的主机路由。
     * @param id 网卡编号。
     * @param add true 添加，false 删除。
     * @return 操作是否成功；失败会记录错误。
     */
    bool route(int id, bool add) const;
    /** @brief 检测链路并按有线优先策略更新路由。 */
    void run();
    std::string host_;
    uint32_t address_ = 0;
    std::vector<int> ports_;
    uint32_t active_address_ = 0;
    int active_ = -1;
    LinkPolicy policy_;
    std::atomic<unsigned> generation_{0};
    std::atomic<bool> ready_{false};
    std::mutex lock_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread worker_;
};
