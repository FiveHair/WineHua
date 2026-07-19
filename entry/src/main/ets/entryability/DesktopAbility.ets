import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { BusinessError, deviceInfo } from '@kit.BasicServicesKit';
import testNapi from 'libentry.so';
import { WineWindowManager } from '../service/WineWindowManager';

const DOMAIN = 0x0000;
const TAG = 'DWA';

export default class DesktopAbility extends UIAbility {
  private toplevelId: number = 0;

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    try {
      const params = want.parameters ?? {};
      this.toplevelId = Number(params.toplevelId) || 0;
      hilog.info(DOMAIN, TAG, '[DWA]  onCreate: tl=%{public}d', this.toplevelId);
      // 注册 context: 运行时切回多窗口模式时 manager 需要 terminateSelf 本 Ability
      WineWindowManager.getInstance().registerDesktopAbility(this.context);
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[DWA] ERR onCreate: %{public}s', JSON.stringify(e));
    }
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    hilog.info(DOMAIN, TAG, '[DWA] onWindowStageCreate tl=%{public}d', this.toplevelId);

    // PC (2in1): 最大化放在 loadContent 之前 — XComponent surface 直接就拿到
    // 最大化后的工作区尺寸, notifyToplevelResize 传给 explorer 的即是正确尺寸
    const isPC = deviceInfo.deviceType === '2in1';
    if (isPC) {
      try {
        const mw = windowStage.getMainWindowSync();
        if (mw) {
          mw.maximize(window.MaximizePresentation.EXIT_IMMERSIVE);
          try { mw.setWindowTitleMoveEnabled(false); } catch (_) {}
          try { mw.setWindowTitleButtonVisible(false, false, false); } catch (_) {}
        }
      } catch (e) {
        hilog.error(DOMAIN, TAG, '[DWA] PC maximize failed: %{public}s', JSON.stringify(e));
      }
    }

    windowStage.loadContent('pages/DesktopWindow', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, '[DWA] loadContent FAILED: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, TAG, '[DWA] loadContent OK tl=%{public}d', this.toplevelId);

      // Pad: 沉浸全屏 + 隐藏状态栏/导航栏 (依赖 loadContent 后的窗口状态)
      if (!isPC) {
        try {
          const mw = windowStage.getMainWindowSync();
          if (mw) {
            mw.setWindowLayoutFullScreen(true).catch((e: BusinessError) => {
              hilog.error(DOMAIN, TAG, '[DWA] fullScreen failed: %{public}s', JSON.stringify(e));
            });
            mw.setSpecificSystemBarEnabled('status', false).catch((e: BusinessError) => {
              hilog.error(DOMAIN, TAG, '[DWA] statusBar failed: %{public}s', JSON.stringify(e));
            });
            mw.setSpecificSystemBarEnabled('navigationIndicator', false).catch((e: BusinessError) => {
              hilog.error(DOMAIN, TAG, '[DWA] navBar failed: %{public}s', JSON.stringify(e));
            });
          }
        } catch (e) {
          hilog.error(DOMAIN, TAG, '[DWA] pad immersive ERR: %{public}s', JSON.stringify(e));
        }
      }
    });
  }

  onForeground(): void {
    hilog.info(DOMAIN, TAG, '[DWA] onForeground tl=%{public}d', this.toplevelId);
  }

  onNewWant(want: Want): void {
    const newId = Number(want.parameters?.toplevelId) || 0;
    hilog.info(DOMAIN, TAG, '[DWA] onNewWant tl: %{public}d -> %{public}d', this.toplevelId, newId);
    this.toplevelId = newId;
  }

  onBackground(): void {
    hilog.info(DOMAIN, TAG, '[DWA] onBackground tl=%{public}d', this.toplevelId);
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, '[DWA] onDestroy tl=%{public}d', this.toplevelId);
    try {
      testNapi.destroyToplevel(this.toplevelId);
      WineWindowManager.getInstance().unregisterAbility(this.toplevelId);
      WineWindowManager.getInstance().unregisterDesktopAbility();
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[DWA] ERR onDestroy: %{public}s', JSON.stringify(e));
    }
  }
}
