#include "compositor_blit.h"
#include <algorithm>
#include <cstring>
#include <vector>

void BlitScaled(uint8_t* dst, int rootW, int rootH,
                const uint8_t* src, int srcStride, int srcW, int srcH,
                int dstX, int dstY, int dstW, int dstH, bool alphaBlend,
                int clipX, int clipY, int clipW, int clipH)
{
    if (!dst || !src || srcStride <= 0 || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
    if (srcW > srcStride) srcW = srcStride;

    int x0 = std::max(0, dstX), y0 = std::max(0, dstY);
    int x1 = std::min(rootW, dstX + dstW), y1 = std::min(rootH, dstY + dstH);
    if (clipW > 0 && clipH > 0) {
        x0 = std::max(x0, clipX); y0 = std::max(y0, clipY);
        x1 = std::min(x1, clipX + clipW); y1 = std::min(y1, clipY + clipH);
    }
    if (x1 <= x0 || y1 <= y0) return;

    const int span = x1 - x0;
    const int64_t stepX = (static_cast<int64_t>(srcW) << 16) / dstW;
    const int64_t stepY = (static_cast<int64_t>(srcH) << 16) / dstH;
    const int64_t maxFx = static_cast<int64_t>(srcW - 1) << 16;
    const int64_t maxFy = static_cast<int64_t>(srcH - 1) << 16;

    /*
     * 分离式双线性插值 (水平插值缓存 + 垂直加权): 原实现每输出像素做
     * 4 次带权采样 (w00..w11 四元组); 分离后行内先做插值 (相邻源行为
     * 一组, 一次计算两行水平结果), 垂直方向加权复用 — 放大时相邻输出行
     * 共享源行 (sy0/sy1 相同), 水平插值缓存直接复用, 内层每像素降为
     * 2 乘法/通道。与旧算法的代数等价性: 分配律展开 (p00*wx0+p01*wx1)*wy0
     * + (p10*wx0+p11*wx1)*wy1 == 原 w00..w11 组合, 中间无截断 (16.8 定点
     * 全程 unsigned), 逐位输出与旧版完全一致 — 局部/整帧合成路径沿用
     * 同一像素值, 无切换误差。
     */
    // 列权重预计算 (原版同算法)。热路径 (每帧调用), static thread_local
    // 复用缓冲避免每帧分配。
    static thread_local std::vector<int> sx0, sx1, wxa, wxb;
    if ((int)sx0.size() < span) {
        sx0.resize(span); sx1.resize(span); wxa.resize(span); wxb.resize(span);
    }
    for (int i = 0; i < span; ++i) {
        int64_t fx = static_cast<int64_t>(x0 + i - dstX) * stepX + (stepX >> 1) - (1 << 15);
        fx = std::max<int64_t>(0, std::min(maxFx, fx));
        sx0[i] = static_cast<int>(fx >> 16);
        sx1[i] = std::min(sx0[i] + 1, srcW - 1);
        wxb[i] = static_cast<int>((fx >> 8) & 0xFF);
        wxa[i] = 256 - wxb[i];
    }

    static thread_local std::vector<unsigned> hrow0, hrow1;  // 16.8 定点, 4 通道 B,G,R,A 平面
    const size_t bufElems = static_cast<size_t>(span) * 4;
    if (hrow0.size() < bufElems) { hrow0.resize(bufElems); hrow1.resize(bufElems); }
    int lastSy0 = -1, lastSy1 = -1;

    for (int y = y0; y < y1; ++y) {
        int64_t fy = static_cast<int64_t>(y - dstY) * stepY + (stepY >> 1) - (1 << 15);
        fy = std::max<int64_t>(0, std::min(maxFy, fy));
        const int sy = static_cast<int>(fy >> 16);
        const int sy1 = std::min(sy + 1, srcH - 1);
        const unsigned wy1 = static_cast<unsigned>((fy >> 8) & 0xFF);
        const unsigned wy0 = 256 - wy1;

        if (sy != lastSy0 || sy1 != lastSy1) {
            const uint8_t* row0 = src + static_cast<size_t>(sy) * srcStride * 4;
            const uint8_t* row1 = src + static_cast<size_t>(sy1) * srcStride * 4;
            for (int i = 0; i < span; ++i) {
                const unsigned wx0 = static_cast<unsigned>(wxa[i]);
                const unsigned wx1 = static_cast<unsigned>(wxb[i]);
                const uint8_t* p00 = row0 + sx0[i] * 4;
                const uint8_t* p01 = row0 + sx1[i] * 4;
                const uint8_t* p10 = row1 + sx0[i] * 4;
                const uint8_t* p11 = row1 + sx1[i] * 4;
                const size_t o = static_cast<size_t>(i) * 4;
                hrow0[o + 0] = static_cast<unsigned>(p00[0]) * wx0 + static_cast<unsigned>(p01[0]) * wx1;
                hrow0[o + 1] = static_cast<unsigned>(p00[1]) * wx0 + static_cast<unsigned>(p01[1]) * wx1;
                hrow0[o + 2] = static_cast<unsigned>(p00[2]) * wx0 + static_cast<unsigned>(p01[2]) * wx1;
                hrow0[o + 3] = static_cast<unsigned>(p00[3]) * wx0 + static_cast<unsigned>(p01[3]) * wx1;
                hrow1[o + 0] = static_cast<unsigned>(p10[0]) * wx0 + static_cast<unsigned>(p11[0]) * wx1;
                hrow1[o + 1] = static_cast<unsigned>(p10[1]) * wx0 + static_cast<unsigned>(p11[1]) * wx1;
                hrow1[o + 2] = static_cast<unsigned>(p10[2]) * wx0 + static_cast<unsigned>(p11[2]) * wx1;
                hrow1[o + 3] = static_cast<unsigned>(p10[3]) * wx0 + static_cast<unsigned>(p11[3]) * wx1;
            }
            lastSy0 = sy; lastSy1 = sy1;
        }
        uint8_t* drow = dst + (static_cast<size_t>(y) * rootW + x0) * 4;
        for (int i = 0; i < span; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            uint8_t* dpx = drow + i * 4;
            const unsigned b = (hrow0[o + 0] * wy0 + hrow1[o + 0] * wy1) >> 16;
            const unsigned g = (hrow0[o + 1] * wy0 + hrow1[o + 1] * wy1) >> 16;
            const unsigned r = (hrow0[o + 2] * wy0 + hrow1[o + 2] * wy1) >> 16;
            if (!alphaBlend) {
                dpx[0] = static_cast<uint8_t>(b);
                dpx[1] = static_cast<uint8_t>(g);
                dpx[2] = static_cast<uint8_t>(r);
                dpx[3] = 255;
                continue;
            }
            const unsigned a = (hrow0[o + 3] * wy0 + hrow1[o + 3] * wy1) >> 16;
            if (a == 0) continue;
            if (a >= 255) {
                dpx[0] = static_cast<uint8_t>(b);
                dpx[1] = static_cast<uint8_t>(g);
                dpx[2] = static_cast<uint8_t>(r);
                dpx[3] = 255;
            } else {
                const unsigned inv = 255 - a;
                const unsigned nb = b + (dpx[0] * inv) / 255;
                const unsigned ng = g + (dpx[1] * inv) / 255;
                const unsigned nr = r + (dpx[2] * inv) / 255;
                dpx[0] = static_cast<uint8_t>(std::min(nb, 255u));
                dpx[1] = static_cast<uint8_t>(std::min(ng, 255u));
                dpx[2] = static_cast<uint8_t>(std::min(nr, 255u));
                dpx[3] = 255;
            }
        }
    }
}

// 收敛自三份内联行循环 (desktop_compositor.cpp blitToplevel / blitSubsurface /
// blitWindowSubsurface 普通分支), 逐字节搬原实现, 行为不变。
// 注: BlitScaled 的 alpha 混合分支也是 SrcOnly 语义 (src + dst*inv/255 + clamp
// + dp[3]=255), 因带双线性权重未一并收敛, 仅记录于此。
void BlitClipAlpha(uint8_t* dstRow, const uint8_t* srcRow, int copyW,
                   bool alphaBlend, PixelBlend blend)
{
    if (!alphaBlend) {
        std::memcpy(dstRow, srcRow, static_cast<size_t>(copyW) * 4);
        return;
    }
    if (blend == PixelBlend::SrcOnly) {
        for (int x = 0; x < copyW; x++) {
            const uint8_t* sp = srcRow + x * 4;
            uint8_t* dp = dstRow + x * 4;
            const uint8_t a = sp[3];
            if (a == 0) continue;
            if (a == 255) {
                std::memcpy(dp, sp, 4);
            } else {
                const unsigned inv = 255 - a;
                const unsigned b = sp[0] + (dp[0] * inv) / 255;
                const unsigned g = sp[1] + (dp[1] * inv) / 255;
                const unsigned r = sp[2] + (dp[2] * inv) / 255;
                dp[0] = b > 255 ? 255 : b;
                dp[1] = g > 255 ? 255 : g;
                dp[2] = r > 255 ? 255 : r;
                dp[3] = 255;
            }
        }
    } else {
        for (int x = 0; x < copyW; x++) {
            const uint8_t* sp = srcRow + x * 4;
            uint8_t* dp = dstRow + x * 4;
            const uint8_t a = sp[3];
            if (a == 0) continue;
            if (a == 255) {
                std::memcpy(dp, sp, 4);
            } else {
                const unsigned inv = 255 - a;
                dp[0] = (sp[0] * a + dp[0] * inv) / 255;
                dp[1] = (sp[1] * a + dp[1] * inv) / 255;
                dp[2] = (sp[2] * a + dp[2] * inv) / 255;
            }
        }
    }
}
