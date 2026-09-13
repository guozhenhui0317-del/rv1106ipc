#include "annexb.hpp"

namespace annexb {

/*
 * 从 Rockchip VENC 输出的 Annex-B 码流中提取解码参数集。
 *
 * VENC 数据由 00 00 01 或 00 00 00 01 起始码分隔。FFmpeg muxer 在写
 * RTSP SDP/FLV header 前需要 H.264 SPS+PPS 或 H.265 VPS+SPS+PPS，因此
 * 首个关键帧到达时扫描这些 NAL，并保持原始 Annex-B 格式存入 extradata。
 * 普通 slice/SEI 不属于全局参数，故不复制，避免 header 随帧内容膨胀。
 */
/**
 * @brief 扫描 Annex-B 并提取 SPS/PPS/VPS。
 *
 * @param[in] data 待处理数据的首地址。
 * @param[in] size 待处理数据的字节数。
 * @param[in] h265 是否按 H.265 解析；false 表示 H.264。
 *
 * @return 参数集数据和位掩码。
 */
ParameterSets parameter_sets(const uint8_t *data, size_t size, bool h265) {
    ParameterSets out;
    size_t pos = 0;
    while (pos + 4 < size) {
        size_t start = size;
        size_t prefix = 0;
        // 找到start code
        for (size_t i = pos; i + 3 < size; ++i) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
                start = i; prefix = 3; break;
            }
            if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 &&
                data[i + 2] == 0 && data[i + 3] == 1) {
                start = i; prefix = 4; break;
            }
        }
        if (start == size || start + prefix >= size)
            break;
        size_t end = size;
        for (size_t i = start + prefix; i + 3 < size; ++i) {
            if (data[i] == 0 && data[i + 1] == 0 &&
                (data[i + 2] == 1 || (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1))) {
                end = i; break;
            }
        }
        /* H.264 NAL 类型位于低 5 位；H.265 位于第一个字节的 bit[6:1]。 */
        const uint8_t type = !h265
                                 ? data[start + prefix] & 0x1f // h264
                                 : (data[start + prefix] >> 1) & 0x3f; // h265
        const unsigned flag = !h265
                                  ? (type == 7 ? 1u : type == 8 ? 2u : 0u)
                                  : (type == 32 ? 1u : type == 33 ? 2u : type == 34 ? 4u : 0u);
        if (flag && !(out.found & flag)) {
            out.data.append(reinterpret_cast<const char *>(data + start), end - start);
            out.found |= flag;
        }
        pos = end;
    }
    return out;
}

} // namespace annexb
