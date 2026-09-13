#pragma once
#include <string>
#include <istream>
#include <sstream>
#include <cstdint>
#include <net/route.h>

/** @brief 判断残留路由是否与本程序所有权记录严格一致。
 * @param target INI 中的目标 IPv4。
 * @param device 内核路由使用的网卡。
 * @param saved_target /run 所有权记录中的目标。
 * @param saved_device /run 所有权记录中的网卡。
 * @param metric 内核显示的优先级，4270 专用于本程序。
 * @param gateway 内核路由的网关值，直连必须为零。
 * @param flags 内核路由标志。
 * @return true 允许持有进程单实例锁的调用者接管，否则拒绝。
 */
inline bool owned_stream_route(const std::string &target, const std::string &device,
                               const std::string &saved_target, const std::string &saved_device,
                               unsigned long metric, unsigned long gateway, unsigned long flags) {
    return target == saved_target && device == saved_device &&
           (device == "eth0" || device == "wlan0") && metric == 4270 && gateway == 0 &&
           flags == (RTF_UP | RTF_HOST);
}

/** @brief 从路由表快照检查当前本程序的目标路由。
 * @param table /proc/net/route 文本流。@param address 目标网络字节序 IPv4。
 * @param target 目标文本。@param active 已知归属网卡，-1 尚无路由。
 * @return 0/1 表示匹配接口；-1 不存在；-2 读取失败、格式错误或外部冲突。
 */
inline int stream_route_state(std::istream &table, uint32_t address,
                              const std::string &target, int active) {
    std::string line;
    if (!std::getline(table, line)) return -2;
    int found = -1;
    while (std::getline(table, line)) {
        std::istringstream row(line);
        std::string dev;
        unsigned long dst, gateway, flags, refs, use, metric, mask;
        if (!(row >> dev >> std::hex >> dst >> gateway >> flags >> std::dec >> refs >> use
                  >> metric >> std::hex >> mask)) return -2;
        if (dst != address || mask != 0xffffffffUL) continue;
        if (active < 0 || found >= 0 || !owned_stream_route(target, dev, target,
                active == 0 ? "eth0" : "wlan0", metric, gateway, flags)) return -2;
        found = active;
    }
    return table.bad() ? -2 : found;
}
