#include "annexb.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

int main() {
    // 混合三/四字节起始码；忽略 IDR、SEI 和重复 SPS。
    const uint8_t h264[] = {
        0, 0, 0, 1, 0x67, 0x64, 0, 0x1f,
        0, 0, 1, 0x68, 0xee, 0x3c, 0x80,
        0, 0, 1, 0x65, 0x88,
        0, 0, 1, 0x67, 0x55,
        0, 0, 1, 0x06, 0xff};
    const auto avc = annexb::parameter_sets(h264, sizeof(h264), false);
    assert(avc.complete(false) && avc.found == 3);
    assert(avc.data.size() == 15);
    assert(std::memcmp(avc.data.data(), h264, 15) == 0);
    assert(!annexb::parameter_sets(h264, 8, false).complete(false));

    const uint8_t h265[] = {
        0, 0, 1, 0x40, 1, 0xaa,  // VPS
        0, 0, 1, 0x42, 1, 0xbb,  // SPS
        0, 0, 1, 0x44, 1, 0xcc,  // PPS
        0, 0, 1, 0x26, 1, 0xdd}; // IDR
    const auto hevc = annexb::parameter_sets(h265, sizeof(h265), true);
    assert(hevc.complete(true) && hevc.found == 7);
    assert(hevc.data.size() == 18);
    assert(std::memcmp(hevc.data.data(), h265, 18) == 0);
    assert(!annexb::parameter_sets(h265, 12, true).complete(true));

    const uint8_t no_start[] = {0x67, 0x64, 0, 0x1f, 0xff};
    assert(annexb::parameter_sets(no_start, sizeof(no_start), false).data.empty());
    assert(annexb::parameter_sets(nullptr, 0, false).data.empty());
    // 每个短前缀都使用精确分配的缓冲区，便于 ASan 检查截断输入越界。
    for (size_t size = 1; size <= sizeof(h264); ++size) {
        const std::vector<uint8_t> truncated(h264, h264 + size);
        const auto result = annexb::parameter_sets(
            truncated.data(), size, false);
        assert(result.data.size() <= size);
    }
    std::cout << "Annex-B tests passed\n";
}
