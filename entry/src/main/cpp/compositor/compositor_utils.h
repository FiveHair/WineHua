#pragma once
#include <cstdint>
#include <vector>
#include <wayland-server-core.h>
#include "compositor_constants.h"
// 像素合成原语 (BlitScaled / BlitClipAlpha / PixelBlend) 在
// compositor_blit.h — 纯函数独立文件, 宿主机单元测试直接编译真实实现。
#include "compositor_blit.h"

// 保比例适配 (letterbox) 几何已迁至 geometry.h (FitRect / ComputeFitRect)。

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
