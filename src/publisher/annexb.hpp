#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace annexb {

struct ParameterSets {
    std::string data;
    unsigned found = 0;

    /**
     * @brief 判断参数集是否满足指定编码格式。
     *
     * @param[in] h265 是否按 H.265 检查；false 表示 H.264。
     *
     * @return true 表示完整，false 表示缺少参数集。
     */
    bool complete(bool h265) const {
        return (found & (h265 ? 0x7u : 0x3u)) ==
               (h265 ? 0x7u : 0x3u);
    }
};

// data 指向 size 字节的有效缓冲区；size 为 0 时允许 nullptr。
// 只提取当前缓冲区内的参数集，不跨帧累积，不验证参数集语法。
ParameterSets parameter_sets(const uint8_t *data, size_t size, bool h265);

} // namespace annexb
