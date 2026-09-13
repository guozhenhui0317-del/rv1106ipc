#include "route_identity.hpp"
#include <cassert>

/** @brief 拒绝无记录、异目标/网卡/优先级/网关的用户路由。 @return 0 表示通过。 */
int main() {
    assert(owned_stream_route("a", "eth0", "a", "eth0", 4270, 0, RTF_UP | RTF_HOST));
    assert(owned_stream_route("a", "wlan0", "a", "wlan0", 4270, 0, RTF_UP | RTF_HOST));
    assert(!owned_stream_route("a", "eth0", "", "", 4270, 0, 5));
    assert(!owned_stream_route("a", "eth0", "b", "eth0", 4270, 0, 5));
    assert(!owned_stream_route("a", "eth0", "a", "wlan0", 4270, 0, 5));
    assert(!owned_stream_route("a", "usb0", "a", "usb0", 4270, 0, 5));
    assert(!owned_stream_route("a", "eth0", "a", "eth0", 0, 0, 5));
    assert(!owned_stream_route("a", "eth0", "a", "eth0", 4270, 1, 5));
    assert(!owned_stream_route("a", "eth0", "a", "eth0", 4270, 0, RTF_UP));
    // 与 ARM/Linux 路由表一致的网络字节序目标值；无真实网络操作。
    const char *header = "Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT\n";
    const char *owned = "eth0 6500A8C0 00000000 0005 0 0 4270 FFFFFFFF 0 0 0\n";
    std::istringstream absent(header);
    assert(stream_route_state(absent, 0x6500a8c0, "192.168.0.101", 0) == -1);
    std::istringstream valid(std::string(header) + owned);
    assert(stream_route_state(valid, 0x6500a8c0, "192.168.0.101", 0) == 0);
    std::istringstream unknown(std::string(header) + owned);
    assert(stream_route_state(unknown, 0x6500a8c0, "192.168.0.101", -1) == -2);
    std::istringstream wrong_device(std::string(header) + owned);
    assert(stream_route_state(wrong_device, 0x6500a8c0, "192.168.0.101", 1) == -2);
    std::istringstream duplicate(std::string(header) + owned + owned);
    assert(stream_route_state(duplicate, 0x6500a8c0, "192.168.0.101", 0) == -2);
    std::istringstream unreadable;
    assert(stream_route_state(unreadable, 0x6500a8c0, "192.168.0.101", 0) == -2);
    std::istringstream corrupt(std::string(header) + "malformed\n");
    assert(stream_route_state(corrupt, 0x6500a8c0, "192.168.0.101", 0) == -2);
    std::istringstream external(std::string(header) +
        "eth0 6500A8C0 00000000 0005 0 0 0 FFFFFFFF 0 0 0\n");
    assert(stream_route_state(external, 0x6500a8c0, "192.168.0.101", 0) == -2);
}
