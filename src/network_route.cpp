#define LOG_TAG "network"
#include "network_route.hpp"
#include "route_identity.hpp"
#include "log.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {
const char *const interfaces[] = {"eth0", "wlan0"};
constexpr int kRouteMetric = 4271; // ioctl 使用实际 metric + 1。

/** @brief 自动关闭探测和路由操作使用的 socket。 */
struct Socket final {
    int fd;
    /** @brief 创建 IPv4 socket。 @param type SOCK_STREAM 或 SOCK_DGRAM。 */
    explicit Socket(int type) : fd(socket(AF_INET, type | SOCK_CLOEXEC, 0)) {}
    /** @brief 释放文件描述符。 */
    ~Socket() { if (fd >= 0) close(fd); }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
};
} // namespace

NetworkRoute::NetworkRoute(const std::string &host, const std::vector<int> &ports)
    : host_(host), ports_(ports) {
    if (inet_pton(AF_INET, host.c_str(), &address_) != 1 || ports.empty() || ports.size() > 4)
        throw std::runtime_error("network failover requires one IPv4 receiver and valid TCP port");
    for (int port : ports_)
        if (port <= 0 || port > 65535) throw std::runtime_error("invalid probe port");
    if (geteuid() != 0)
        throw std::runtime_error("network failover requires root for device binding and routing");
    // ServiceState 已持有单实例锁；只接管上次本程序留下的精确匹配路由。
    std::string saved_target, saved_device;
    std::ifstream owner("/run/gzh_ipc.route");
    owner >> saved_target >> saved_device;
    std::ifstream table("/proc/net/route");
    if (!table) throw std::runtime_error("cannot inspect /proc/net/route");
    std::string line;
    std::getline(table, line);
    while (std::getline(table, line)) {
        std::istringstream row(line);
        std::string dev;
        unsigned long dst, gateway, flags, refs, use, metric, mask;
        if (row >> dev >> std::hex >> dst >> gateway >> flags >> std::dec >> refs >> use
                >> metric >> std::hex >> mask) {
            if (dst == address_ && mask == 0xffffffffUL) {
                if (active_ >= 0 || !owned_stream_route(host, dev, saved_target, saved_device,
                                                       metric, gateway, flags))
                    throw std::runtime_error("unowned host route to " + host +
                                             "; inspect it or disable network:enable_failover");
                active_ = dev == "eth0" ? 0 : 1;
                LOG_WARN("recovering owned route %s/32 dev %s after prior exit", host.c_str(), dev.c_str());
            }
        }
    }
    worker_ = std::thread(&NetworkRoute::run, this);
}

NetworkRoute::~NetworkRoute() {
    {
        std::lock_guard<std::mutex> guard(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::ifstream table("/proc/net/route");
    if (active_ >= 0 && stream_route_state(table, address_, host_, active_) == active_)
        route(active_, false);
}

bool NetworkRoute::reachable(int id, uint32_t &local_address) const {
    local_address = 0;
    Socket sock(SOCK_STREAM | SOCK_NONBLOCK);
    if (sock.fd < 0) return false;
    ifreq request{};
    std::strncpy(request.ifr_name, interfaces[id], IFNAMSIZ - 1);
    if (ioctl(sock.fd, SIOCGIFFLAGS, &request) < 0 ||
        !(request.ifr_flags & IFF_UP) || !(request.ifr_flags & IFF_RUNNING)) return false;
    if (ioctl(sock.fd, SIOCGIFADDR, &request) < 0) return false;
    sockaddr_in local{};
    std::memcpy(&local, &request.ifr_addr, sizeof(local));
    local.sin_port = 0;
    local_address = local.sin_addr.s_addr;
    if (ioctl(sock.fd, SIOCGIFNETMASK, &request) < 0) return false;
    sockaddr_in mask{};
    std::memcpy(&mask, &request.ifr_netmask, sizeof(mask));
    // 本需求接收端在两块网卡的同一局域网内；不能把跨网关目标当作直连主机。
    if ((local.sin_addr.s_addr & mask.sin_addr.s_addr) !=
        (address_ & mask.sin_addr.s_addr)) return false;
    // 每个接口总探测预算约 700 ms；多个端口平分，避免失联时串行放大等待。
    for (int port : ports_) {
        Socket probe(SOCK_STREAM | SOCK_NONBLOCK);
        if (probe.fd < 0) continue;
        if (setsockopt(probe.fd, SOL_SOCKET, SO_BINDTODEVICE, interfaces[id],
                      std::strlen(interfaces[id]) + 1) < 0 ||
            bind(probe.fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) continue;
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_addr.s_addr = address_;
        peer.sin_port = htons(port);
        if (connect(probe.fd, reinterpret_cast<sockaddr *>(&peer), sizeof(peer)) == 0) return true;
        if (errno == ECONNREFUSED) return true; // 拒绝连接也是目标可达，不因服务停机切网。
        if (errno != EINPROGRESS) continue;
        pollfd pending{probe.fd, POLLOUT, 0};
        if (poll(&pending, 1, 700 / ports_.size()) <= 0) continue;
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(probe.fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
            (error == 0 || error == ECONNREFUSED)) return true;
    }
    return false;
}

bool NetworkRoute::route(int id, bool add) const {
    // 先记录再添加；进程被中断后可恢复。/run 在重启后清空，正常无路由时记录无效。
    if (add) {
        std::ofstream owner("/run/gzh_ipc.route", std::ios::trunc);
        owner << host_ << ' ' << interfaces[id] << '\n';
        owner.flush();
        if (!owner) {
            LOG_ERROR("cannot record stream route ownership; refusing route change");
            return false;
        }
    }
    Socket sock(SOCK_DGRAM);
    rtentry entry{};
    sockaddr_in dst{}, mask{};
    dst.sin_family = mask.sin_family = AF_INET;
    dst.sin_addr.s_addr = address_;
    mask.sin_addr.s_addr = 0xffffffffU;
    std::memcpy(&entry.rt_dst, &dst, sizeof(dst));
    std::memcpy(&entry.rt_genmask, &mask, sizeof(mask));
    entry.rt_flags = RTF_UP | RTF_HOST;
    entry.rt_metric = kRouteMetric;
    entry.rt_dev = const_cast<char *>(interfaces[id]);
    if (sock.fd >= 0 && ioctl(sock.fd, add ? SIOCADDRT : SIOCDELRT, &entry) == 0) return true;
    // 网卡被系统关闭时，内核可能已自动删除路由。
    if (sock.fd >= 0 && !add && errno == ESRCH) return true;
    LOG_ERROR("%s route %s/32 dev %s failed: %s", add ? "add" : "delete",
              host_.c_str(), interfaces[id], std::strerror(errno));
    return false;
}

void NetworkRoute::run() {
    LOG_INFO("failover enabled: eth0 preferred, wlan0 backup, receiver=%s, probe_ports=%zu",
             host_.c_str(), ports_.size());
    int64_t retry_at = 0;
    while (true) {
        uint32_t addresses[2]{};
        const bool wired = reachable(0, addresses[0]);
        const bool wifi = reachable(1, addresses[1]);
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        const int next = policy_.choose(active_, wired, wifi, now_ms);
        std::ifstream table("/proc/net/route");
        int installed = stream_route_state(table, address_, host_, active_);
        const bool conflict = installed == -2;
        // 没有可达候选时不反复创建/切换；第三次失败后仅暂停发布。
        const bool candidate = next >= 0 && (next == 0 ? wired : wifi);
        if (candidate && !conflict && (next != active_ || installed != active_) && now_ms >= retry_at) {
            const int old = active_;
            ready_ = false;
            ++generation_;
            retry_at = now_ms + 3000;
            if (installed < 0 || route(installed, false)) {
                if (route(next, true)) {
                    active_ = next;
                    LOG_INFO("stream route %s -> %s, target=%s (switch or missing-route repair)",
                             old < 0 ? "none" : interfaces[old], interfaces[next], host_.c_str());
                } else if (old < 0 || !route(old, true)) {
                    active_ = -1;
                }
            }
            std::ifstream updated("/proc/net/route");
            installed = stream_route_state(updated, address_, host_, active_);
        }
        const bool available = active_ >= 0 && installed == active_ && policy_.usable(active_);
        const uint32_t source = active_ >= 0 ? addresses[active_] : 0;
        const bool ip_changed = available && source && active_address_ && source != active_address_;
        if (available != ready_.load() || ip_changed) {
            ready_ = false;
            ++generation_; // 同网卡换 IP、掉线/恢复也使所有 RTSP/RTMP 旧连接失效。
            LOG_INFO("stream network %s, interface=%s, source_changed=%d, generation=%u",
                     available ? "ready" : "unavailable",
                     active_ < 0 ? "none" : interfaces[active_], ip_changed, generation());
        }
        if (available && source) active_address_ = source;
        ready_ = available;
        if (conflict && now_ms >= retry_at) {
            LOG_WARN("cannot verify owned route to %s; publishing paused, external routes untouched", host_.c_str());
            retry_at = now_ms + 30000;
        }
        std::unique_lock<std::mutex> guard(lock_);
        if (wake_.wait_for(guard, std::chrono::seconds(1), [this] { return stop_; })) break;
    }
}
