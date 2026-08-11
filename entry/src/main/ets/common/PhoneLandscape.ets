import { window } from '@kit.ArkUI';

/**
 * Run initialization only after a phone window has acquired a landscape
 * rectangle. Wine must not see the device's natural portrait size during
 * desktop creation because that becomes its initial wl_output mode.
 */
export function runAfterPhoneLandscape(
  windowStage: window.WindowStage,
  onReady: () => void,
  onFailure: (message: string) => void
): void {
  let mainWindow: window.Window;
  try {
    mainWindow = windowStage.getMainWindowSync();
  } catch (err) {
    onFailure(`get main window failed: ${JSON.stringify(err)}`);
    onReady();
    return;
  }

  let started = false;
  const sizeChanged = (size: window.Size): void => {
    if (size.width > size.height) finish();
  };
  const finish = (): void => {
    if (started) return;
    started = true;
    mainWindow.off('windowSizeChange', sizeChanged);
    onReady();
  };
  const isLandscape = (): boolean => {
    const rect = mainWindow.getWindowProperties().windowRect;
    return rect.width > rect.height;
  };

  try {
    mainWindow.on('windowSizeChange', sizeChanged);
    mainWindow.setPreferredOrientation(window.Orientation.LANDSCAPE).then(() => {
      if (isLandscape()) {
        finish();
      }
    }).catch((err: Error) => {
      mainWindow.off('windowSizeChange', sizeChanged);
      onFailure(`landscape orientation failed: ${JSON.stringify(err)}`);
      finish();
    });
  } catch (err) {
    onFailure(`landscape orientation setup failed: ${JSON.stringify(err)}`);
    finish();
  }
}
