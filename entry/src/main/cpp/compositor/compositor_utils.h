#pragma once
#include <cstdint>
#include <vector>
#include <wayland-server-core.h>
#include "compositor_constants.h"

// 保比例适配 (letterbox) 几何已迁至 geometry.h (FitRect / ComputeFitRect)。

// 双线性缩放 blit (16.16 固定点)。clipX/Y/W/H: 可选目标裁剪矩形 (root 坐标,
// 0/0/0/0 = 不裁剪)。裁剪只约束写入范围, 采样相位仍由 dstX/dstY/dstW/dstH
// 整矩形决定 — 局部合成 (DamageRect) 与整帧合成逐像素一致。
void BlitScaled(uint8_t* dst, int rootW, int rootH,
                const uint8_t* src, int srcStride, int srcW, int srcH,
                int dstX, int dstY, int dstW, int dstH, bool alphaBlend,
                int clipX = 0, int clipY = 0, int clipW = 0, int clipH = 0);

// 像素混合的两种历史语义 (收敛自三份内联行循环, 保留差异 — 见各调用点):
// SrcOnly: 源不乘 alpha 直接叠加到背景, 结果 clamp 到 255, 目标 alpha
//   强制 255 — blitToplevel 普通分支 (窗口帧对半透明底色的处理);
// Normal: 标准 alpha 混合 (src*a + dst*inv)/255, 结果为加权平均不超 255,
//   目标 alpha 不变 — blitSubsurface / blitWindowSubsurface 普通分支。
enum class PixelBlend { SrcOnly, Normal };

// 单行 ARGB 拷贝/混合。dstRow/srcRow 已对齐到裁剪后的行起点, copyW 为
// 裁剪后像素数。alphaBlend=false → 整行不透明 memcpy (含 alpha 通道)。
void BlitClipAlpha(uint8_t* dstRow, const uint8_t* srcRow, int copyW,
                   bool alphaBlend, PixelBlend blend);

// 最小化自动恢复: Wine 没有 unset_minimized 协议, 还原时直接 commit
// 正常尺寸内容, 而最小化标题栏只 commit ~200x30 的小表面 (定期刷新)。
// 大于阈值的 commit 判定为真实窗口恢复。
inline bool IsRestoreSizeCommit(bool minimized, int32_t contentW, int32_t contentH) {
    return minimized && contentW > compositor_consts::kRestoreMinContentWidth &&
           contentH > compositor_consts::kRestoreMinContentHeight;
}

// -- Surface 工具 --

uint64_t MakeSurfaceKey(uint32_t clientPid, uint32_t surfaceId);
uint32_t GetWaylandClientPid(wl_client* client);
