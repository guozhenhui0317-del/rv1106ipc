#pragma once

extern "C" {
#include "iniparser.h"
}
#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>

namespace config {
/** @brief 报告配置键错误，不输出可能含密码的值。
 * @param key 配置键。@param reason 错误原因。@return 不返回，抛出异常。
 */
inline void fail(const std::string &key, const char *reason) {
    throw std::runtime_error("config " + key + ": " + reason);
}
/** @brief 读取非空配置字符串。
 * @param d 字典。@param key 键。@param fallback 缺省值，NULL 表示必填。
 * @return 配置副本；缺失或为空时抛异常。
 */
inline std::string text(const dictionary *d, const std::string &key, const char *fallback = nullptr) {
    const char *value = iniparser_getstring(d, key.c_str(), fallback);
    if (!value || !*value) fail(key, "missing or empty");
    return value;
}
/** @brief 严格解析十进制整数，拒绝溢出、尾随字符和范围错误。
 * @param value 文本。@param key 错误定位。@param low 下界。@param high 上界。
 * @return 整数值；非法时抛异常。
 */
inline int integer(const std::string &value, const std::string &key, int low, int high) {
    if (value.empty() || value.find_first_not_of("-0123456789") != std::string::npos)
        fail(key, "expected decimal integer");
    // 官方读取器使用 base=0；拒绝前导零，避免预检按十进制、运行按八进制解释。
    const size_t first = value[0] == '-' ? 1 : 0;
    if (value.size() > first + 1 && value[first] == '0')
        fail(key, "leading zero is ambiguous; use decimal without padding");
    char *end = nullptr;
    errno = 0;
    const long n = std::strtol(value.c_str(), &end, 10);
    if (errno || end == value.c_str() || *end || n < low || n > high)
        fail(key, "integer out of range or malformed");
    return static_cast<int>(n);
}
/** @brief 读取并校验整数配置。
 * @param d 字典。@param key 键。@param low 下界。@param high 上界。
 * @param fallback 缺省文本；NULL 表示必填。@return 合法整数。
 */
inline int number(const dictionary *d, const std::string &key, int low, int high,
                  const char *fallback = nullptr) {
    return integer(text(d, key, fallback), key, low, high);
}
/** @brief 校验枚举配置，避免错误拼写静默退回其他模式。
 * @param d 字典。@param key 键。@param values 合法值。@param fallback 缺省值。
 * @return 无返回值；非法时抛异常。
 */
inline void choice(const dictionary *d, const std::string &key,
                   std::initializer_list<const char *> values, const char *fallback = nullptr) {
    const auto value = text(d, key, fallback);
    for (const char *candidate : values) if (value == candidate) return;
    fail(key, "unsupported value");
}
/** @brief 校验实际媒体链路使用的关键参数；不访问硬件、网络或写文件。
 * @param d 已解析字典。@return 无返回值；非法时抛出带键名的异常。
 * @details 安全数值上界用于拒绝异常配置，不代表 SDK 支持其范围内所有组合。
 * ISP 专有参数及模型内容仍由 SDK 校验，不承诺检测所有被截断但语法合法的 INI。
 */
inline void validate(const dictionary *d) {
    if (!d) fail("file", "not loaded");
    number(d, "log:level", 0, 3, "2");
    const int rtsp = number(d, "stream:enable_rtsp", 0, 1);
    const int rtmp = number(d, "stream:enable_rtmp", 0, 1);
    if (!rtsp && !rtmp) fail("stream", "enable at least one protocol");
    const int failover = number(d, "network:enable_failover", 0, 1, "0");
    number(d, "stream:io_timeout_ms", 100, 60000, "5000");
    choice(d, "stream:rtsp_transport", {"tcp", "udp"}, "tcp");
    number(d, "video.source:enable_aiq", 0, 1, "1");
    number(d, "video.source:enable_vo", 0, 0, "0");
    const int npu = number(d, "video.source:enable_npu", 0, 1);
    number(d, "osd:enable_fps", 0, 1, "1");
    std::string receiver;
    for (const char *protocol : {"rtsp", "rtmp"}) {
        if ((std::string(protocol) == "rtsp" ? rtsp : rtmp) == 0) continue;
        std::string previous;
        for (const char *stream : {"main", "ai"}) {
            const std::string key = "stream:" + std::string(protocol) + "_" + stream + "_url";
            const auto url = text(d, key);
            const std::string prefix = std::string(protocol) + "://";
            if (url.compare(0, prefix.size(), prefix) || url.size() > 1023 ||
                url.find_first_of(" \t\r\n") != std::string::npos)
                fail(key, "invalid URL scheme, length or whitespace");
            const auto slash = url.find('/', prefix.size());
            if (slash == std::string::npos || slash + 1 == url.size()) fail(key, "host and stream path required");
            auto host = url.substr(prefix.size(), slash - prefix.size());
            const auto at = host.rfind('@');
            if (at != std::string::npos) host.erase(0, at + 1);
            if (host.empty()) fail(key, "empty host");
            // 非切换模式保留 FFmpeg 对域名/IPv6 的支持；方括号 IPv6 单独处理。
            if (host.front() == '[') {
                const auto close = host.find(']');
                in6_addr address;
                if (close == std::string::npos ||
                    inet_pton(AF_INET6, host.substr(1, close - 1).c_str(), &address) != 1)
                    fail(key, "invalid IPv6 host");
                if (close + 1 < host.size()) {
                    if (host[close + 1] != ':') fail(key, "invalid authority");
                    integer(host.substr(close + 2), key, 1, 65535);
                }
                host = host.substr(0, close + 1);
            } else {
                const auto colon = host.find(':');
                if (colon != std::string::npos) {
                    integer(host.substr(colon + 1), key, 1, 65535);
                    host.resize(colon);
                }
                if (host.empty()) fail(key, "empty host");
            }
            if (failover) {
                in_addr address;
                if (inet_pton(AF_INET, host.c_str(), &address) != 1 ||
                    (!receiver.empty() && receiver != host))
                    fail(key, "failover requires one common IPv4 receiver");
                receiver = host;
            }
            if (url == previous) fail(key, "main and AI must use different URLs");
            previous = url;
        }
    }
    const int isp_fps = number(d, "isp.0.adjustment:fps", 1, 120, "25");
    for (int id = 0; id < 2; ++id) {
        const std::string base = "video." + std::to_string(id) + ":";
        const int w = number(d, base + "width", 2, 8192);
        const int h = number(d, base + "height", 2, 8192);
        if ((w & 1) || (h & 1)) fail(base, "NV12 dimensions must be even");
        number(d, base + "max_width", w, 8192);
        number(d, base + "max_height", h, 8192);
        choice(d, base + "output_data_type", {"H.264", "H.265"});
        if (rtmp && text(d, base + "output_data_type") != "H.264") fail(base, "RTMP requires H.264");
        choice(d, base + "rc_mode", {"CBR", "VBR"}, "CBR");
        choice(d, base + "rc_quality", {"highest", "higher", "high", "medium", "low", "lower", "lowest"}, "high");
        number(d, base + "enable_refer_buffer_share", 0, 1, "1");
        choice(d, base + "h264_profile", {"baseline", "main", "high"}, "high");
        const int src = number(d, base + "src_frame_rate_num", 1, 120, "25");
        const int sd = number(d, base + "src_frame_rate_den", 1, 120, "1");
        const int dst = number(d, base + "dst_frame_rate_num", 1, 120, "25");
        const int dd = number(d, base + "dst_frame_rate_den", 1, 120, "1");
        if (dst < dd || dst % dd || dst * sd > src * dd || dst > isp_fps * dd)
            fail(base, "VI requires integer output FPS within source/ISP FPS");
        number(d, base + "gop", 1, 10000, "50");
        number(d, base + "input_buffer_count", 1, 32);
        number(d, base + "buffer_count", 1, 32, "4");
        number(d, base + "buffer_size", 1024, 64 * 1024 * 1024);
        const int max = number(d, base + "max_rate", 1, 1000000);
        const int mid = number(d, base + "mid_rate", 1, max);
        number(d, base + "min_rate", 0, mid, "0");
    }
    if (number(d, "audio.0:enable", 0, 1)) {
        choice(d, "audio.0:encode_type", {"G711A"}, "G711A");
        choice(d, "audio.0:format", {"S16"}, "S16");
        number(d, "audio.0:sample_rate", 8000, 8000, "8000");
        number(d, "audio.0:channels", 1, 1, "1");
        number(d, "audio.0:frame_size", 1152, 1152, "1152");
        number(d, "audio.0:volume", 0, 100, "50");
        if (number(d, "audio.0:enable_vqe", 0, 1, "1")) text(d, "audio.0:vqe_cfg");
    }
    if (npu) {
        number(d, "video.source:npu_fps", 1, 120, "10");
        for (const char *axis : {"width", "height"}) {
            const int n = number(d, "video.1:" + std::string(axis), 2, 8192);
            number(d, "video.2:" + std::string(axis), n, n);
        }
        number(d, "osd.common:normalized_screen_width", 1, 8192);
        number(d, "osd.common:normalized_screen_height", 1, 8192);
        choice(d, "event.regional_invasion:rockiva_model_type", {"small", "medium", "big"}, "small");
        number(d, "event.regional_invasion:enabled", 0, 1, "0");
        number(d, "event.regional_invasion:sensitivity_level", 1, 100, "50");
        number(d, "event.regional_invasion:time_threshold", 0, INT_MAX / 1000, "1");
        number(d, "event.regional_invasion:proportion", 0, 100, "5");
        text(d, "rockiva:model_path");
    }
}
} // namespace config
