import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0x0000;
const STORE_NAME: string = 'desktop_mode';
const KEY_MODE: string = 'mode';
export const MODE_VIRTUAL: string = 'virtual';   // 虚拟桌面: 桌面合成器 + 独立窗口
export const MODE_FUSION: string = 'fusion';     // 融合模式: 每窗口独立 OHOS 窗口
const DEFAULT_MODE: string = MODE_FUSION;

// PC 桌面模式 (仅 2in1/PC 生效, Pad/phone 恒走虚拟桌面不受此影响):
// 持久化用户选择。生效时机: 仅应用启动 — WineWindowManager.init 读此值
// 判定 isDesktopMode (explorer 是否启动/桌面合成器是否开启), 运行中改值
// 不影响当前会话, 故本服务不提供 subscribe, 设置页改完自行刷新显示即可
// (与 DisplaySettingsService 同款, 见其"重启生效"约定)。
export class DesktopModeService {
  private static instance: DesktopModeService | null = null;

  static getInstance(): DesktopModeService {
    if (!DesktopModeService.instance) {
      DesktopModeService.instance = new DesktopModeService();
    }
    return DesktopModeService.instance;
  }

  mode: string = DEFAULT_MODE;

  private store: preferences.Preferences | null = null;
  private initialized: boolean = false;

  init(ctx: common.Context): void {
    if (this.initialized) {
      return;
    }
    this.initialized = true;
    try {
      this.store = preferences.getPreferencesSync(ctx, { name: STORE_NAME });
      const v = this.store.getSync(KEY_MODE, DEFAULT_MODE) as string;
      if (v === MODE_VIRTUAL || v === MODE_FUSION) {
        this.mode = v;
      }
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'desktop mode load fail: %{public}s', JSON.stringify(e));
    }
  }

  setMode(v: string): void {
    if (v !== MODE_VIRTUAL && v !== MODE_FUSION) {
      return;
    }
    this.mode = v;
    if (!this.store) {
      return;
    }
    try {
      this.store.putSync(KEY_MODE, this.mode);
      this.store.flush();
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'desktop mode persist fail: %{public}s', JSON.stringify(e));
    }
  }
}
