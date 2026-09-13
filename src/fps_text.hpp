#pragma once
#include <cstdint>
#include <cstring>

namespace fps_text {
/**
 * @brief 绘制 10x14 像素字形，2BPP 白字，未绘制区域保持透明。
 * @param pixels 画布首地址。
 * @param width 虚拟宽度，须为 4 的倍数。
 * @param height 虚拟高度。
 * @param text 文字，仅支持 FPS、冒号、小数点、空格和数字。
 * @return 无返回值；无效参数忽略，超出画布部分裁剪。
 */
inline void draw(uint8_t *pixels, unsigned width, unsigned height, const char *text) {
    if (!pixels || !text || !width || width % 4 || !height) return;
    static const uint8_t glyphs[][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
        {31,16,16,30,16,16,16}, // F
        {30,17,17,30,16,16,16}, // P
        {15,16,16,14,1,1,30},  // S
        {0,4,4,0,4,4,0},       // :
        {0,0,0,0,0,4,4}        // .
    };
    const unsigned stride = width / 4;
    std::memset(pixels, 0, static_cast<size_t>(stride) * height); // 00 为透明像素。
    for (unsigned n = 0; text[n] && n < width / 12; ++n) {
        const char ch = text[n];
        const int index = ch >= '0' && ch <= '9' ? ch - '0' :
                          ch == 'F' ? 10 : ch == 'P' ? 11 : ch == 'S' ? 12 :
                          ch == ':' ? 13 : ch == '.' ? 14 : -1;
        if (index < 0) continue;
        for (unsigned y = 0; y < 7; ++y)
            for (unsigned x = 0; x < 5; ++x)
                if (glyphs[index][y] & (1U << (4 - x)))
                    for (unsigned dy = 0; dy < 2; ++dy) {
                        const unsigned row = 4 + y * 2 + dy;
                        const unsigned col = 4 + n * 12 + x * 2;
                        // 每个字形点占两个像素；设备对照验证：高半字节在左，低半字节在右。
                        if (row < height && col + 1 < width)
                            pixels[static_cast<size_t>(row) * stride + col / 4] |=
                                static_cast<uint8_t>(0x0fU << (4 - (col % 4) * 2));
                    }
    }
}
} // namespace fps_text
