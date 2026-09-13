#include "network_route.hpp"
#include "fps_text.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

/** @brief 验证有线优先、失败门限、稳定回切和离线不振荡。 @return 0 表示通过。 */
int main() {
    LinkPolicy policy;
    assert(policy.choose(-1, false, false, 0) == -1);
    assert(policy.choose(-1, true, true, 1000) == 0);
    assert(policy.choose(0, false, true, 2000) == 0);
    assert(policy.choose(0, false, true, 3000) == 0);
    assert(policy.choose(0, false, true, 4000) == 1);
    assert(policy.choose(1, false, false, 5000) == 1);
    assert(policy.choose(1, true, true, 6000) == 1);
    assert(policy.choose(1, true, true, 15999) == 1);
    assert(policy.choose(1, true, true, 16000) == 0);
    assert(policy.choose(0, false, true, 17000) == 0);
    assert(policy.choose(0, true, true, 18000) == 0);
    assert(policy.choose(0, false, true, 19000) == 0);
    assert(policy.choose(1, true, false, 20000) == 1); // 单次 Wi-Fi 抖动不回切。
    assert(policy.choose(0, false, false, 21000) == 0);
    LinkPolicy wifi_only;
    assert(wifi_only.choose(-1, false, true, 0) == 1);
    assert(wifi_only.choose(1, true, true, 1000) == 1);
    assert(wifi_only.choose(1, false, true, 10000) == 1);
    assert(wifi_only.choose(1, true, true, 11000) == 1);
    assert(wifi_only.choose(1, true, true, 20000) == 1);
    assert(wifi_only.choose(1, true, true, 21000) == 0);

    LinkPolicy failures;
    assert(failures.choose(-1, false, true, 0) == 1);
    assert(failures.choose(1, true, false, 1000) == 1);
    assert(failures.usable(1));
    assert(failures.choose(1, true, false, 2000) == 1);
    assert(failures.choose(1, true, false, 3000) == 0);
    assert(!failures.usable(1) && failures.usable(0));
    for (int i = 0; i < 10; ++i)
        assert(failures.choose(0, false, false, 4000 + i * 1000) == 0);
    assert(!failures.usable(0) && !failures.usable(1) && !failures.usable(-1));
    assert(failures.choose(0, true, false, 15000) == 0);
    assert(failures.usable(0));

    // 保护字节和小画布验证裁剪；每个支持字形都必须产生白色像素。
    for (unsigned width : {4U, 16U, 128U, 256U}) {
        for (unsigned height : {1U, 16U, 32U, 48U}) {
            const size_t bytes = width / 4 * height;
            std::vector<uint8_t> pixels(bytes + 2, 0x42);
            fps_text::draw(pixels.data() + 1, width, height, "FPS: 999.9 overflow");
            assert(pixels.front() == 0x42 && pixels.back() == 0x42);
        }
    }
    std::vector<uint8_t> pixels(256 * 48 / 4);
    for (const char *ch = "0123456789FPS:."; *ch; ++ch) {
        const char text[] = {*ch, 0};
        fps_text::draw(pixels.data(), 256, 48, text);
        assert(std::count_if(pixels.begin(), pixels.end(), [](uint8_t p) { return p != 0; }) > 0);
        for (uint8_t p : pixels) assert(p == 0 || p == 0x0f || p == 0xf0 || p == 0xff);
    }
    fps_text::draw(pixels.data(), 256, 48, " ");
    assert(std::count(pixels.begin(), pixels.end(), 0) == static_cast<int>(pixels.size()));
    fps_text::draw(pixels.data(), 256, 48, "F");
    assert(pixels[4 * 64 + 1] == 0xff); // F 顶部四个白色像素。
    assert(pixels[6 * 64 + 1] == 0xf0); // 设备验证：F 左竖占高半字节。
    for (unsigned y = 18; y < 48; ++y)
        for (unsigned x = 0; x < 64; ++x) assert(pixels[y * 64 + x] == 0);
    // 按高位在左解包整张 F 字形，避免仅验证非零而漏掉笔画错位。
    const unsigned rows[7] = {31, 16, 16, 30, 16, 16, 16};
    for (unsigned y = 0; y < 48; ++y)
        for (unsigned x = 0; x < 256; ++x) {
            const bool ink = y >= 4 && y < 18 && x >= 4 && x < 14 &&
                (rows[(y - 4) / 2] & (1U << (4 - (x - 4) / 2)));
            const unsigned pixel = (pixels[y * 64 + x / 4] >> (6 - 2 * (x % 4))) & 3;
            assert(pixel == (ink ? 3U : 0U));
        }
    fps_text::draw(nullptr, 256, 48, "FPS: 0.0");
    uint8_t guard = 0x42;
    fps_text::draw(&guard, 3, 48, "F");
    assert(guard == 0x42);
    std::puts("Network policy and FPS canvas tests passed");
}
