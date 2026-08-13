import { hilog } from '@kit.PerformanceAnalysisKit';
import { DesktopLayer } from '../components/DesktopLayer';

const DOMAIN = 0x0000;
const TAG = 'VD';

// PC 虚拟桌面页面 (VirtualDesktopAbility 承载): 极简壳 — 桌面全部逻辑
// (renderer 生命周期 / 触控状态机 / 触控板 / 鼠标 / 键盘 / 滚轮) 复用
// components/DesktopLayer (feature/main-ui 桌面层组件, 原 master
// DesktopWindow.ets 的迁入进化版)。不带手柄/虚拟键盘 overlay:
// PC 有物理键鼠, 不需要 Pad 的悬浮按钮。
@Entry
@Component
struct VirtualDesktop {
  // 桌面分辨率恒等于屏幕/scale (WineWindowManager.init 的 setOutputSize):
  // desktopLauncherVisible 保持默认 true → DesktopLayer.onSurfaceChanged
  // 跳过 root resize, 窗口只是显示口 (任何尺寸 letterbox 等比缩放),
  // 用户缩放/最大化窗口不改变 Wine 桌面布局。
  // Index.onLauncherVisibleChange 在 desktopSeparateWindow 时不写回此值,
  // 不会干扰本窗口语义。
  aboutToAppear(): void {
    hilog.info(DOMAIN, TAG, '[VD] aboutToAppear');
  }

  build() {
    Stack() {
      // floating=false: 独立窗口内无悬浮窗态, 输入全直通
      // 无手柄/虚拟键盘悬浮按钮 (用户决策: PC 虚拟桌面不带 Pad 的
      // GamepadOverlay/KeyboardOverlay — 有物理键鼠)
      DesktopLayer({ floating: false })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
  }
}
