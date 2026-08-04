import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { fileIo as fs, fileUri, picker } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { SmokeLaunchRequest, SmokeRunner, defaultSmokeRequest, parseSmokeRequest,
  stageManagedVkd3dRuntime, syncManagedSmoke } from '../service/SmokeRunner';
import { WineWindowManager } from '../service/WineWindowManager';

const DOMAIN = 0x0000;
const SOCK = '/data/storage/el2/base/files/.wine/wine-wayland';
const FILES_BASE = '/data/storage/el2/base/files';

// Wine 运行时从 rawfile 解压到 files/
const WINE_ROOT = `${FILES_BASE}/wine`;
const WINE_BIN = `${WINE_ROOT}/bin`;
const WINE_LIB = `${WINE_BIN}/x86_64-unix`;
const BIN_DIR = WINE_BIN;
const BOX64 = `${WINE_BIN}/box64`;

const DRIVE_C = '/data/storage/el2/base/files/.wine/drive_c';
const WINE_SMOKE_PREFIX = `${FILES_BASE}/.wine-smoke`;
const WINE_AUTOMATION_HOME = `${FILES_BASE}/home`;

function pathExists(path: string): boolean {
  try {
    return fs.accessSync(path);
  } catch (_) {
    return false;
  }
}

function readTextIfExists(path: string): string {
  try {
    return fs.readTextSync(path, { encoding: 'utf-8' }).trim();
  } catch (_) {
    return '';
  }
}

function writeTextFile(path: string, text: string): void {
  const file = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
  try {
    fs.writeSync(file.fd, text);
  } finally {
    fs.closeSync(file);
  }
}

function ensureDirectory(path: string): boolean {
  try {
    if (!pathExists(path)) {
      fs.mkdirSync(path, true);
    }
    return pathExists(path);
  } catch (e) {
    hilog.warn(DOMAIN, 'wine', 'mkdir failed path=%{public}s err=%{public}s', path, JSON.stringify(e));
    return false;
  }
}

function removePathRecursive(path: string): void {
  let st: fs.Stat;
  try {
    st = fs.lstatSync(path);
  } catch (e) {
    hilog.warn(DOMAIN, 'wine', 'removePath: skip missing path=%{public}s err=%{public}s', path, JSON.stringify(e));
    return;
  }

  if (st.isSymbolicLink() || !st.isDirectory()) {
    fs.unlinkSync(path);
    return;
  }

  const names = fs.listFileSync(path);
  for (const name of names) {
    removePathRecursive(`${path}/${name}`);
  }
  fs.rmdirSync(path);
}

interface ProcessInfo {
  pid: number;
  name: string;
  path: string;
  state: string;
}

@Entry
@Component
struct Index {
  @StorageLink('winehua.smoke.request') smokeRequestText: string = '';
  @StorageLink('winehua.d3d.backend') d3dBackendText: string = 'dxvk_legacy';
  @StorageLink('winehua.d3d.preference') d3dPreferenceText: string = 'dxvk_legacy';
  @StorageLink('winehua.game.path') gamePathText: string = '';
  @StorageLink('winehua.game.args') gameArguments: string[] = [];
  @StorageLink('winehua.experiment.names') experimentNames: string[] = [];
  @StorageLink('winehua.experiment.hashes') experimentHashes: string[] = [];
  @StorageLink('winehua.game.d3d_env_keys') d3dEnvKeys: string[] = [];
  @StorageLink('winehua.game.d3d_env_values') d3dEnvValues: string[] = [];
  @StorageLink('winehua.game.click_title_prefix') gameClickTitlePrefix: string = '';
  @StorageLink('winehua.game.click_button_text') gameClickButtonText: string = '';
  @StorageLink('winehua.game.click_delay_ms') gameClickDelayMs: number = 1500;
  @StorageLink('winehua.game.click_client_x_permille') gameClickClientXPermille: number = -1;
  @StorageLink('winehua.game.click_client_y_permille') gameClickClientYPermille: number = -1;
  @StorageLink('winehua.perf.profile') perfProfileText: string = 'baseline';
  @State state: string = 'idle';
  @State prefixReady: boolean = false;
  @State statusText: string = '检测中...';
  @State curDir: string = DRIVE_C;
  @State entries: string[] = [];
  @State dirStack: string[] = [];
  @State processList: ProcessInfo[] = [];
  @State initOverlayVisible: boolean = false;
  @State initOverlayText: string = '';
  @State initInProgress: boolean = false;
  private hasRawfileWineData: boolean = false;
  private zHome: string = '/storage/Users/currentUser/Download';
  private automationMode: boolean = false;
  private prefixMode: string = 'reuse';
  private prefixRoot: string = `${FILES_BASE}/.wine`;
  private driveC: string = DRIVE_C;
  private sockPath: string = SOCK;
  private smokeRequest: SmokeLaunchRequest = defaultSmokeRequest();
  private smokeStarted: boolean = false;
  private cleanPrefixPrepared: boolean = false;
  @State d3dBackend: string = 'dxvk_legacy';
  private gameTestMode: boolean = false;
  private gameStarted: boolean = false;
  private experimentMode: boolean = false;
  private experimentStaged: boolean = false;
  private pendingVkd3dSmoke: boolean = false;

  aboutToAppear(): void {
    this.smokeRequest = parseSmokeRequest(this.smokeRequestText);
    this.automationMode = this.smokeRequest.mode === 'smoke';
    this.experimentMode = this.smokeRequest.mode === 'experiment';
    this.gameTestMode = this.smokeRequest.mode === 'game' || this.experimentMode;
    this.experimentStaged = false;
    this.d3dBackend = this.d3dBackendText === 'dxvk_legacy' ||
      this.d3dBackendText === 'dxvk_modern_2_6' ||
      this.d3dBackendText === 'vkd3d_limited_500k' ? this.d3dBackendText : 'wined3d';
    this.prefixMode = this.smokeRequest.prefixMode;
    this.prefixRoot = this.prefixMode === 'clean' ? WINE_SMOKE_PREFIX : `${FILES_BASE}/.wine`;
    this.driveC = `${this.prefixRoot}/drive_c`;
    this.sockPath = `${this.prefixRoot}/wine-wayland`;
    this.curDir = this.driveC;
    this.hasRawfileWineData = this.detectRawfileWineData();

    testNapi.setStateCallback((s: string) => {
      hilog.info(DOMAIN, 'wine', 'state: %{public}s', s);

      const colonIdx = s.indexOf(':');
      const pid = colonIdx >= 0 ? parseInt(s.substring(0, colonIdx)) : -1;
      const stateMsg = colonIdx >= 0 ? s.substring(colonIdx + 1) : s;
      this.state = stateMsg;

      if (stateMsg == 'wineserver-starting' || stateMsg == 'wineboot-starting') {
        this.initInProgress = true;
        this.setInitStatus('Wine 正在初始化...', true);
      } else if (stateMsg == 'wine-ready') {
        // C++ publishes wine-ready only after wineboot's durable completion
        // marker. Confirm the prefix once more before exposing the desktop.
        this.setInitStatus('正在确认 Wine 环境...', true);
        this.waitForPrefix();
      } else if (stateMsg == 'wine-running') {
        this.refreshProcessList();
        if (!this.initInProgress) this.setInitStatus('运行中', false);
      } else if (stateMsg == 'exited') {
        this.refreshProcessList();
        const running = this.processList.filter(p => p.state === 'running');
        if (!this.initInProgress && this.prefixReady && running.length === 0) {
          this.setInitStatus('已就绪', false);
        }
      } else if (stateMsg == 'wine-failed' || stateMsg == 'wineboot-failed' ||
        stateMsg == 'wineserver-failed') {
        this.initInProgress = false;
        const failureText = stateMsg == 'wineboot-failed' ? 'Wine 初始化失败' :
          (stateMsg == 'wineserver-failed' ? 'Wine 服务启动失败' : '启动失败');
        this.setInitStatus(failureText, false);
        this.refreshProcessList();
      }
    });
    this.refreshPrefix();
    // 下一帧自动初始化 (避免阻塞 UI 首次渲染)
    setTimeout(() => this.doInit(), 0);
  }

  private detectRawfileWineData(): boolean {
    const ctx = getContext(this);
    const rm = ctx.resourceManager;
    try {
      const rawFd = rm.getRawFdSync('wine-data.zip');
      hilog.info(DOMAIN, 'wine', 'rawfile: wine-data.zip exists len=%{public}d', rawFd.length);
      rm.closeRawFdSync('wine-data.zip');
      return true;
    } catch (_) {
      hilog.info(DOMAIN, 'wine', 'rawfile: wine-data.zip not packaged');
      return false;
    }
  }

  private readRawfileText(name: string): string {
    const rm = getContext(this).resourceManager;
    try {
      const rawFd = rm.getRawFdSync(name);
      try {
        const data = new ArrayBuffer(rawFd.length);
        const count = fs.readSync(rawFd.fd, data, { offset: rawFd.offset, length: rawFd.length });
        const bytes = new Uint8Array(data, 0, count);
        let text = '';
        for (let i = 0; i < bytes.length; ++i) text += String.fromCharCode(bytes[i]);
        return text.trim();
      } finally {
        rm.closeRawFdSync(name);
      }
    } catch (_) {
      return '';
    }
  }

  private runtimeNeedsRefresh(): boolean {
    if (!pathExists(`${WINE_ROOT}/share/wine/wine.inf`)) return true;
    const packaged = this.readRawfileText('wine-runtime-manifest.json');
    if (!packaged) return false;
    return packaged !== readTextIfExists(`${WINE_ROOT}/.winehua-runtime-manifest.json`);
  }

  private setInitStatus(text: string, visible: boolean): void {
    this.statusText = text;
    this.initOverlayText = text;
    this.initOverlayVisible = visible;
  }

  // Pad: 从 rawfile 取出 wine-data.zip → zlib.decompressFile 解压到 files/wine/
  // 使用 getRawFdSync 流式读取，避免 249MB 一次加载到内存导致 OOM
  private async extractWineData(): Promise<void> {
    const marker = `${WINE_ROOT}/share/wine/wine.inf`;
    const packagedManifest = this.readRawfileText('wine-runtime-manifest.json');
    hilog.info(DOMAIN, 'wine', 'extract: check marker=%{public}s', marker);
    if (!this.runtimeNeedsRefresh()) {
      hilog.info(DOMAIN, 'wine', 'extract: runtime manifest matches');
      return;
    }
    hilog.info(DOMAIN, 'wine', 'extract: runtime missing or changed, start managed refresh');

    const ctx = getContext(this);
    const rm = ctx.resourceManager;
    const rawFd = rm.getRawFdSync('wine-data.zip');

    try {
      // 流式拷贝: getRawFdSync 获取 fd → 分块读取 → 写入临时文件
      hilog.info(DOMAIN, 'wine', 'extract: rawFd fd=%{public}d offset=%{public}d length=%{public}d',
        rawFd.fd, rawFd.offset, rawFd.length);

      const tmpZip = `${FILES_BASE}/wine-data.zip`;
      hilog.info(DOMAIN, 'wine', 'extract: writing tmpZip=%{public}s (stream)', tmpZip);
      const dst = fs.openSync(tmpZip, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);

      const CHUNK = 4 * 1024 * 1024; // 4MB 分块
      const buf = new ArrayBuffer(CHUNK);
      let pos = rawFd.offset;
      const end = rawFd.offset + rawFd.length;
      let totalWritten = 0;

      while (pos < end) {
        const toRead = Math.min(CHUNK, end - pos);
        const n = fs.readSync(rawFd.fd, buf, { offset: pos, length: toRead });
        if (n <= 0) break;
        fs.writeSync(dst.fd, buf, { length: n });
        pos += n;
        totalWritten += n;
      }
      fs.closeSync(dst);
      hilog.info(DOMAIN, 'wine', 'extract: stream copy ok, totalWritten=%{public}d', totalWritten);

      // files/wine is a managed runtime.  Refresh it atomically at the
      // directory level while keeping .wine and .wine-smoke untouched.
      if (pathExists(WINE_ROOT)) removePathRecursive(WINE_ROOT);
      // 确保目标目录存在
      fs.mkdirSync(WINE_ROOT, true);
      hilog.info(DOMAIN, 'wine', 'extract: calling decompressFile → %{public}s', WINE_ROOT);
      await zlib.decompressFile(tmpZip, WINE_ROOT, {});
      hilog.info(DOMAIN, 'wine', 'extract: decompress ok');

      // 验证
      if (fs.accessSync(marker)) {
        hilog.info(DOMAIN, 'wine', 'extract: verified, marker exists');
        if (packagedManifest) writeTextFile(`${WINE_ROOT}/.winehua-runtime-manifest.json`, packagedManifest);
      } else {
        hilog.warn(DOMAIN, 'wine', 'extract: marker still missing after decompress!');
      }
      this.statusText = '已解压';
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'extract FAIL: %{public}s', JSON.stringify(e));
      this.statusText = '解压失败';
    } finally {
      rm.closeRawFdSync('wine-data.zip');
    }
  }

  private refreshPrefix = (): void => {
    try {
      this.prefixReady = testNapi.checkWinePrefix(this.prefixMode);
      if (this.prefixReady) {
        this.curDir = this.driveC;
        this.dirStack = [];
        this.listDir(this.driveC);
      } else {
        this.entries = [];
      }
    } catch (e) {
      this.prefixReady = false;
      this.entries = [];
    }
  }

  // wineboot 后台异步初始化 prefix，轮询直到 registry 和核心目录完整
  private waitForPrefix = async (): Promise<void> => {
    for (let i = 0; i < 90; i++) {
      try {
        if (testNapi.checkWinePrefix(this.prefixMode)) {
          this.prefixReady = true;
          this.initInProgress = false;
          this.setInitStatus('已就绪', false);
          this.curDir = this.driveC;
          this.dirStack = [];
          this.listDir(this.driveC);
          this.startRequestedSmoke();
          this.startRequestedGame();
          this.startPendingVkd3dSmoke();
          return;
        }
      } catch (e) { /* prefix 还没初始化完成 */ }
      await new Promise<void>(r => setTimeout(r, 1000));
    }
    // 超时，仍然尝试刷新一次
    this.refreshPrefix();
    if (this.prefixReady) {
      this.initOverlayVisible = false;
      this.startRequestedSmoke();
      this.startRequestedGame();
      this.startPendingVkd3dSmoke();
    } else {
      this.initInProgress = false;
      this.setInitStatus('Wine 前缀初始化超时', false);
      hilog.error(DOMAIN, 'wine', 'prefix remained unavailable after initialization timeout');
    }
  }

  private refreshProcessList = (): void => {
    try {
      this.processList = testNapi.getProcessList() as ProcessInfo[];
    } catch (e) {
      this.processList = [];
    }
  }

  private doKillProcess = (pid: number): void => {
    try {
      testNapi.killProcess(pid);
      setTimeout(() => this.refreshProcessList(), 500);
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'killProcess error: %{public}s', JSON.stringify(e));
    }
  }

  private listDir = (path: string): void => {
    try {
      hilog.info(DOMAIN, 'wine', 'listDir: path=%{public}s', path);
      let names = fs.listFileSync(path);
      hilog.info(DOMAIN, 'wine', 'listDir: got %{public}d entries', names.length);
      names.sort((a, b) => {
        let aIsDir = this.isDir(path + '/' + a);
        let bIsDir = this.isDir(path + '/' + b);
        if (aIsDir && !bIsDir) return -1;
        if (!aIsDir && bIsDir) return 1;
        if (!aIsDir && !bIsDir) {
          let aExe = a.toLowerCase().endsWith('.exe') ? 0 : 1;
          let bExe = b.toLowerCase().endsWith('.exe') ? 0 : 1;
          return aExe - bExe || a.localeCompare(b);
        }
        return a.localeCompare(b);
      });
      this.entries = names;
    } catch (e) {
      this.entries = [];
    }
  }

  private isDir = (path: string): boolean => {
    try { return fs.statSync(path).isDirectory(); } catch (_) { return false; }
  }

  private isExe = (name: string): boolean => {
    return name.toLowerCase().endsWith('.exe');
  }

  private windowsParentDirectory(path: string): string {
    const separator = Math.max(path.lastIndexOf('\\'), path.lastIndexOf('/'));
    return separator > 2 ? path.substring(0, separator) : 'C:\\';
  }

  private d3dLaunchEnvironment(): Record<string, string> {
    if (this.d3dBackend.indexOf('dxvk_') !== 0) return {};
    ensureDirectory(`${this.driveC}/smoke/results/desktop-dxvk`);
    const environment: Record<string, string> = {
      'DXVK_LOG_LEVEL': 'warn',
      'DXVK_LOG_PATH': 'C:\\smoke\\results\\desktop-dxvk',
      'WINEDEBUG': '-all',
      /* Venus uses a shared-memory command ring between the x86_64 guest and
       * the ARM64 renderer.  Box64's aggressive weak-barrier mode can violate
       * the x86 ordering expected by the ring and eventually corrupt a random
       * Vulkan command. Keep this scoped to DXVK/Venus processes. */
      'BOX64_DYNAREC_WEAKBARRIER': '0',
      'WINEHUA_PERF_PROFILE': this.perfProfileText
    };
    /* Modern 2.6 baseline runs must not inherit Legacy DXVK diagnostics or
     * compatibility switches. Keep only transport correctness and caller
     * supplied overrides while capturing the stock startup result. */
    if (this.d3dBackend === 'dxvk_modern_2_6') {
      environment['VN_WINEHUA_STRONG_RING_BARRIER'] = '1';
      environment['DXVK_WINEHUA_TRACE_SAMPLED'] = '0';
      environment['DXVK_WINEHUA_TRACE_FLOW'] = '0';
      environment['DXVK_WINEHUA_TRACE_API'] = '0';
      const overrideCount = Math.min(this.d3dEnvKeys.length, this.d3dEnvValues.length);
      for (let index = 0; index < overrideCount; ++index) {
        environment[this.d3dEnvKeys[index]] = this.d3dEnvValues[index];
      }
      return environment;
    }
    if (this.perfProfileText === 'shadow-precise-strong-ring-trace')
      environment['VN_WINEHUA_PRESENT_TRACE'] = '1';
    if (this.perfProfileText === 'direct-fence-wait')
      environment['VN_WINEHUA_DIRECT_FENCE_WAIT'] = '1';
    else if (this.perfProfileText === 'no-remote-sync')
      environment['VN_WINEHUA_REMOTE_MEMORY_SYNC'] = '0';
    else if (this.perfProfileText === 'no-dynamic-flush')
      environment['DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED'] = '0';
    else if (this.perfProfileText === 'fence-feedback')
      environment['VN_PERF'] = '';
    else if (this.perfProfileText === 'shadow-none') {
      environment['VKR_WINEHUA_SHADOW_FROM_HOST'] = 'none';
      environment['VKR_WINEHUA_SHADOW_TRACE'] = '0';
    } else if (this.perfProfileText === 'shadow-trace') {
      environment['VKR_WINEHUA_SHADOW_FROM_HOST'] = 'full';
      environment['VKR_WINEHUA_SHADOW_TRACE'] = '1';
      /* A game Want cannot reliably carry arbitrary env arrays on all
       * Harmony builds. Reuse the explicit diagnostic profile as a bounded
       * trace switch so Heaven render-pass investigation is reproducible. */
      environment['DXVK_WINEHUA_TRACE_SAMPLED'] = '1';
      environment['DXVK_WINEHUA_TRACE_FLOW'] = '1';
    } else if (this.perfProfileText === 'shadow-to-host-explicit') {
      environment['VKR_WINEHUA_SHADOW_FROM_HOST'] = 'to-host-explicit';
      environment['VKR_WINEHUA_SHADOW_TRACE'] = '0';
    } else if (this.perfProfileText === 'shadow-precise' ||
      this.perfProfileText === 'shadow-precise-no-semaphore-feedback-single-ring' ||
      this.perfProfileText === 'shadow-precise-no-semaphore-feedback-single-ring-trace' ||
      this.perfProfileText === 'shadow-full-no-semaphore-feedback-single-ring-trace' ||
      this.perfProfileText === 'shadow-precise-single-ring' ||
      this.perfProfileText === 'shadow-precise-sync-submit' ||
      this.perfProfileText === 'shadow-precise-strong-ring' ||
      this.perfProfileText === 'shadow-precise-legacy-host-sync' ||
      this.perfProfileText === 'shadow-precise-dirty-ring' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-no-upload' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-no-upload-fast' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-coverage-sort' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-coverage-sort-sampled' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-coverage-poll' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-alias-cover' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-serialized' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-descriptor-serialized' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-frame-assoc-trace' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-present-image-trace' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-perf' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-gpu-frame-profile' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-frame-timeline' ||
      this.perfProfileText === 'shadow-precise-strong-ring-trace' ||
      this.perfProfileText === 'shadow-precise-strong-ring-perf' ||
      this.perfProfileText === 'shadow-precise-strong-ring-async-present' ||
      this.perfProfileText === 'shadow-precise-strong-ring-fence-poll' ||
      this.perfProfileText === 'shadow-precise-strong-ring-mailbox' ||
      this.perfProfileText === 'shadow-precise-direct-fence' ||
      this.perfProfileText === 'shadow-precise-retain-shmem' ||
      this.perfProfileText === 'shadow-precise-cpu-upload') {
      environment['DXVK_WINEHUA_PRECISE_SHADOW'] = '1';
      environment['DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED'] = '1';
      environment['VN_WINEHUA_REMOTE_MEMORY_SYNC'] = '1';
      environment['VKR_WINEHUA_SHADOW_FROM_HOST'] =
        this.perfProfileText === 'shadow-full-no-semaphore-feedback-single-ring-trace' ? 'full' : 'precise';
      environment['VKR_WINEHUA_SHADOW_TRACE'] =
        (this.perfProfileText === 'shadow-precise-strong-ring-trace' ||
         this.perfProfileText === 'shadow-precise-no-semaphore-feedback-single-ring-trace' ||
         this.perfProfileText === 'shadow-full-no-semaphore-feedback-single-ring-trace') ? '1' : '0';
      if (this.perfProfileText === 'shadow-precise-single-ring')
        environment['VN_PERF'] = 'no_fence_feedback,no_query_feedback,no_multi_ring';
      if (this.perfProfileText === 'shadow-precise-no-semaphore-feedback-single-ring' ||
        this.perfProfileText === 'shadow-precise-no-semaphore-feedback-single-ring-trace' ||
        this.perfProfileText === 'shadow-full-no-semaphore-feedback-single-ring-trace')
        environment['VN_PERF'] =
          'no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring';
      if (this.perfProfileText === 'shadow-precise-sync-submit')
        environment['VN_PERF'] =
          'no_fence_feedback,no_query_feedback,no_async_queue_submit';
      if (this.perfProfileText === 'shadow-precise-strong-ring' ||
        this.perfProfileText === 'shadow-precise-legacy-host-sync' ||
        this.perfProfileText === 'shadow-precise-dirty-ring' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-no-upload' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-no-upload-fast' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-coverage-sort' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-coverage-sort-sampled' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-coverage-poll' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-alias-cover' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-serialized' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-descriptor-serialized' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-frame-assoc-trace' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-present-image-trace' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-perf' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-gpu-frame-profile' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-frame-timeline' ||
        this.perfProfileText === 'shadow-precise-strong-ring-trace' ||
        this.perfProfileText === 'shadow-precise-strong-ring-perf' ||
        this.perfProfileText === 'shadow-precise-strong-ring-async-present' ||
        this.perfProfileText === 'shadow-precise-strong-ring-fence-poll' ||
        this.perfProfileText === 'shadow-precise-strong-ring-mailbox')
        environment['VN_WINEHUA_STRONG_RING_BARRIER'] = '1';
      if (this.perfProfileText === 'shadow-precise-strong-ring-perf' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-perf')
        environment['VN_WINEHUA_PERF_SUMMARY'] = '1';
      if (this.perfProfileText === 'shadow-precise-direct-fence')
        environment['VN_WINEHUA_DIRECT_FENCE_WAIT'] = '1';
      if (this.perfProfileText === 'shadow-precise-dirty-ring-frame-assoc-trace')
        environment['WINEHUA_DXVK_TRACE_CAMERA'] = '1';
      if (this.perfProfileText === 'shadow-precise-dirty-ring-present-image-trace')
        environment['WINEHUA_DXVK_TRACE_PRESENT_IMAGE'] = '1';
      if (this.perfProfileText === 'shadow-precise-retain-shmem')
        environment['VN_WINEHUA_DEFER_SHMEM_UNREF'] = '1';
      if (this.perfProfileText === 'shadow-precise-cpu-upload')
        environment['VKR_WINEHUA_GPU_UPLOAD'] = '0';
    }
    const overrideCount = Math.min(this.d3dEnvKeys.length, this.d3dEnvValues.length);
    for (let index = 0; index < overrideCount; ++index) {
      environment[this.d3dEnvKeys[index]] = this.d3dEnvValues[index];
    }
    return environment;
  }

  /* The requirements executable is a Windows Vulkan probe, not a DXVK DLL
   * loader. Launch it with the same Guest Vulkan/Venus runtime selected by
   * SmokeRunner so a user-triggered run observes the real PE path. */
  private capabilityProbeEnvironment(windowsExePath: string): Record<string, string> {
    const environment = this.d3dLaunchEnvironment();
    const requiresExperimentVulkan = this.experimentMode &&
      windowsExePath.indexOf('C:\\smoke\\experiments\\') === 0;
    const requiresVkd3dRuntime = this.isVkd3dProfile();
    if (!windowsExePath.toLowerCase().endsWith('winehua_dxvk26_requirements.exe') &&
      !requiresExperimentVulkan && !requiresVkd3dRuntime) {
      return environment;
    }
    const guestVulkanRoot = `${WINE_ROOT}/bin/guest_vulkan`;
    const guestVulkanLibrary = `${guestVulkanRoot}/lib`;
    const guestVulkanIcd = `${guestVulkanRoot}/share/vulkan/icd.d/venus_icd.x86_64.json`;
    const box64LibraryPath = `${guestVulkanLibrary}:${WINE_ROOT}/bin/guest_gfx/lib:` +
      `${WINE_ROOT}/bin:${WINE_ROOT}/bin/x86_64-unix:${WINE_ROOT}/lib/x86_64`;
    environment['WINEHUA_VULKAN_RUNTIME'] = '1';
    environment['WINEHUA_VULKAN_LOADER_ARCH'] = 'x86_64';
    environment['WINEHUA_VENUS_ICD_ARCH'] = 'x86_64';
    environment['USE_LIBBOX64'] = '1';
    environment['BOX64_LD_LIBRARY_PATH'] = box64LibraryPath;
    environment['BOX64_EMULATED_LIBS'] = 'libvulkan.so:libvulkan.so.1:' +
      'libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:' +
      'libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:' +
      'libwayland-client.so:libwayland-client.so.0:libwayland-server.so:' +
      'libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:' +
      'libdrm.so:libdrm.so.2:libffi.so:libffi.so.8';
    environment['VK_DRIVER_FILES'] = guestVulkanIcd;
    environment['VK_ICD_FILENAMES'] = guestVulkanIcd;
    environment['VN_DEBUG'] = 'vtest';
    /* capabilityProbeEnvironment() also serves isolated VKD3D experiments,
     * whose product D3D backend intentionally remains WineD3D.  Those runs
     * therefore bypass the DXVK-only transport defaults in
     * d3dLaunchEnvironment().  Keep Box64's weak-barrier optimization off and
     * publish the Venus ring with a full barrier here as well; otherwise a
     * long-running PE Vulkan stream can eventually expose partially ordered
     * command bytes to the native ARM64 renderer. */
    environment['BOX64_DYNAREC_WEAKBARRIER'] = '0';
    environment['VN_WINEHUA_STRONG_RING_BARRIER'] = '1';
    environment['VN_WINEHUA_REMOTE_MEMORY_SYNC'] = '1';
    /* The vtest bridge has distinct Host and Guest mappings. Venus feedback
     * slots cannot establish a readback dependency here; use the direct
     * device-fence wait below instead of polling an invalid feedback value. */
    environment['VN_PERF'] =
      'no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring';
    environment['VN_WINEHUA_DIRECT_FENCE_WAIT'] = '1';
    environment['VKR_WINEHUA_SHADOW_FROM_HOST'] = 'precise';
    if ((this.experimentMode &&
      this.smokeRequest.experimentId.indexOf('vkd3d-500k-') === 0) ||
      requiresVkd3dRuntime) {
      // Force the hash-verified DLL staged beside the microtest. Without an
      // explicit override Wine may satisfy LoadLibrary("d3d12.dll") with its
      // builtin module, which would invalidate the VKD3D experiment evidence.
      environment['WINEDLLOVERRIDES'] = requiresVkd3dRuntime ? 'd3d12=n' : 'd3d12=n,b';
      environment['VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC'] = '1';
      // The Gate C probe intentionally excludes unsupported query meta
      // pipelines. This profile is never selected for ordinary launches.
      environment['VKD3D_WINEHUA_DISABLE_QUERY_META'] = '1';
    }
    environment['WINEDEBUG'] = '+loaddll,+module';
    return environment;
  }

  private isVkd3dExperiment(): boolean {
    return this.experimentMode &&
      this.smokeRequest.experimentId.indexOf('vkd3d-') === 0;
  }

  private isVkd3dProfile(): boolean {
    return this.d3dBackend === 'vkd3d_limited_500k';
  }

  private d3dPresentBackend(): string {
    // A staged VKD3D DLL presents through the Vulkan/Venus path even though
    // D3D11 remains on WineD3D. The product profile is explicit and default off.
    return this.d3dBackend.indexOf('dxvk_') === 0 || this.isVkd3dExperiment() ||
      this.isVkd3dProfile() ?
      'venus_broker_present' : 'virgl_compositor';
  }

  private selectD3dBackend = (backend: string): void => {
    const selected = backend === 'dxvk_legacy' || backend === 'dxvk_modern_2_6' ||
      backend === 'vkd3d_limited_500k' ? backend : 'wined3d';
    this.d3dBackend = selected;
    this.d3dBackendText = selected;
    // VKD3D is deliberately not persisted as the normal cold-start backend.
    this.d3dPreferenceText = selected === 'vkd3d_limited_500k' ? 'dxvk_legacy' : selected;
    this.statusText = selected === 'dxvk_legacy' ? 'DXVK Legacy' :
      (selected === 'dxvk_modern_2_6' ? 'DXVK Modern 2.6' :
        (selected === 'vkd3d_limited_500k' ? 'VKD3D 2.6 Limited 500K（实验）' : 'WineD3D'));
    hilog.info(DOMAIN, 'GameTest', 'D3D backend selected=%{public}s', selected);
  }

  private enterDir = (name: string): void => {
    let newPath = this.curDir + '/' + name;
    this.dirStack.push(this.curDir);
    this.curDir = newPath;
    this.listDir(newPath);
  }

  private goUp = (): void => {
    let parent = this.dirStack.pop();
    if (parent) {
      this.curDir = parent;
      this.listDir(parent);
    }
  }

  private doInit = async (): Promise<void> => {
    // Automation owns an internal HOME and must never open a Picker.
    if (this.automationMode || this.experimentMode) {
      this.zHome = `${WINE_AUTOMATION_HOME}/`;
    } else if (this.gameTestMode || this.pendingVkd3dSmoke) {
      // Game-test mode avoids the picker but keeps the same managed user home
      // used by the normal desktop session, so Z: points at installed games.
      this.zHome = '/storage/Users/currentUser/Download/app.hackeris.winehua/';
    } else {
      const documentViewPicker = new picker.DocumentViewPicker();
      let documentSaveResult = await documentViewPicker.save({ pickerMode: picker.DocumentPickerMode.DOWNLOAD });
      const pickerHome = new fileUri.FileUri(documentSaveResult[0]).path;
      this.zHome = pickerHome.endsWith('/') ? pickerHome : `${pickerHome}/`;
    }
    // HDC shell cannot remove application-owned sandbox files. A clean smoke
    // prefix must be reset by the App before any server or socket is started.
    if ((this.automationMode || this.experimentMode) && this.prefixMode === 'clean' && !this.cleanPrefixPrepared) {
      this.setInitStatus('正在创建干净测试环境...', true);
      const resetOk = testNapi.resetWinePrefix('clean') as boolean;
      if (!resetOk) {
        this.setInitStatus('干净测试环境重置失败', false);
        hilog.error(DOMAIN, 'SmokeRunner', 'App-owned clean prefix reset failed');
        return;
      }
      this.cleanPrefixPrepared = true;
      this.prefixReady = false;
      hilog.info(DOMAIN, 'SmokeRunner', 'App-owned clean prefix reset completed');
    }
    hilog.info(DOMAIN, 'wine', 'auto-init: Z home=%{public}s', this.zHome);
    const gamesDir = `${this.zHome}games`;
    if (!ensureDirectory(gamesDir)) {
      this.setInitStatus('创建 games 目录失败', false);
      return;
    }
    hilog.info(DOMAIN, 'wine', 'auto-init: games directory=%{public}s', gamesDir);
    // 检查是否需要解压 wine 数据文件
    hilog.info(DOMAIN, 'wine', 'auto-init: check marker=%{public}s', `${WINE_ROOT}/share/wine/wine.inf`);
    let needExtract = this.runtimeNeedsRefresh();
    if (needExtract) {
      this.setInitStatus('解压数据...', true);
      hilog.info(DOMAIN, 'wine', 'auto-init: extracting wine data...');
      await this.extractWineData();
    }
    /* C:\smoke lives inside the persistent Wine prefix, not inside the
     * replaceable files/wine runtime. Refresh only this manifest-owned tree so
     * overwrite installs cannot keep running stale test binaries. */
    if (!syncManagedSmoke(this.prefixMode)) {
      this.setInitStatus('测试程序同步失败', false);
      hilog.error(DOMAIN, 'wine', 'auto-init: managed C:\\smoke synchronization failed');
      return;
    }
    hilog.info(DOMAIN, 'wine', 'auto-init: starting Wine...');
    this.initInProgress = true;
    this.setInitStatus('启动服务...', true);
    /* The renderer process is created while startServer() initializes the
     * broker, before the isolated PE receives capabilityProbeEnvironment().
     * Make the VKD3D transport selection before vtest starts. Manual VKD3D
     * activation stops and recreates the graphics broker first, while normal
     * launches retain the validated DXVK/WineD3D host profile. */
    const hostShadowProfile = this.isVkd3dExperiment() || this.isVkd3dProfile()
      ? 'shadow-precise-direct-fence' : this.perfProfileText;
    testNapi.setHostShadowProfile(hostShadowProfile);
    hilog.info(DOMAIN, 'Experiment', 'host shadow profile=%{public}s experiment=%{public}s',
      hostShadowProfile, this.smokeRequest.experimentId);
    const ok: boolean = testNapi.startServer(this.sockPath);
    if (!ok) {
      this.initInProgress = false;
      this.setInitStatus('启动服务失败', false);
      return;
    }
    testNapi.launchClient(BOX64, [], this.sockPath, `${BIN_DIR}:${WINE_LIB}`, this.zHome,
      this.automationMode || this.experimentMode, this.prefixMode, this.d3dBackend);
  }

  private doReset = (): void => {
    this.setInitStatus('正在清空...', true);
    try {
      testNapi.resetWinePrefix(this.prefixMode);
      this.state = 'idle';
      this.prefixReady = false;
      this.processList = [];
      this.entries = [];
      this.setInitStatus('已清空', false);
      setTimeout(() => this.doInit(), 500);
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'reset error: %{public}s', JSON.stringify(e));
      this.setInitStatus('清空失败', false);
    }
  }

  private doRun = (name: string): void => {
    this.statusText = '启动中...';
    let exePath = this.curDir + '/' + name;
    testNapi.runWineProgram({
      windowsExePath: exePath,
      argv: [],
      environment: this.capabilityProbeEnvironment(exePath),
      workingDirectory: this.curDir,
      prefixMode: this.prefixMode,
      d3dBackend: this.d3dBackend,
      presentBackend: this.d3dPresentBackend(),
      automationMode: this.experimentMode
    });
  }

  private relPath = (): string => {
    if (this.curDir == this.driveC) return 'C:\\';
    return 'C:\\' + this.curDir.substring(this.driveC.length + 1).replace(/\//g, '\\');
  }

  private startRequestedSmoke = (): void => {
    // automationMode also selects the managed-window boundary used by the
    // manual VKD3D probe. Only an explicit smoke request may start the full
    // core suite; otherwise the suite races the standalone D3D12 process in
    // the same clean prefix and tears down its session after a few seconds.
    if (this.smokeRequest.mode !== 'smoke' || this.smokeStarted) return;
    this.smokeStarted = true;
    const runner = new SmokeRunner(this.smokeRequest, (text: string) => {
      this.statusText = text;
      this.initOverlayText = text;
    }, this.perfProfileText);
    runner.run().catch((error: Object) => {
      this.statusText = 'Smoke: infrastructure failure';
      hilog.error(DOMAIN, 'SmokeRunner', 'suite failed: %{public}s', JSON.stringify(error));
    });
  }

  private startRequestedGame = (): void => {
    if (!this.gameTestMode || this.gameStarted || this.gamePathText.length === 0) return;
    this.gameStarted = true;
    let gamePath = this.gamePathText;
    const gameArguments = this.gameArguments.slice();
    if (this.experimentMode) {
      if (!this.smokeRequest.experimentId || !this.smokeRequest.experimentExecutable ||
        !testNapi.stageExperimentPayload(this.smokeRequest.experimentId,
          this.experimentNames.slice(), this.experimentHashes.slice(), this.prefixMode,
          this.smokeRequest.experimentSourceUrl)) {
        this.setInitStatus('实验载荷校验或导入失败', false);
        hilog.error(DOMAIN, 'Experiment', 'payload staging failed id=%{public}s',
          this.smokeRequest.experimentId);
        return;
      }
      this.experimentStaged = true;
      gamePath = `C:\\smoke\\experiments\\${this.smokeRequest.experimentId}\\` +
        this.smokeRequest.experimentExecutable;
      if (this.smokeRequest.experimentResultName.length > 0) {
        gameArguments.push('--result', `C:\\smoke\\experiments\\${this.smokeRequest.experimentId}` +
          `\\results\\${this.smokeRequest.experimentResultName}.json`);
      }
    }
    this.statusText = `Launching ${this.d3dBackend}: ${gamePath}`;
    const result = testNapi.runWineProgram({
      windowsExePath: gamePath,
      // @StorageLink arrays are observable proxies. Pass a plain array across
      // the N-API boundary so napi_is_array can enumerate every game argument.
      argv: gameArguments,
      environment: this.capabilityProbeEnvironment(gamePath),
      workingDirectory: this.windowsParentDirectory(gamePath),
      prefixMode: this.prefixMode,
      d3dBackend: this.d3dBackend,
      presentBackend: this.d3dPresentBackend(),
      automationMode: this.experimentMode
    }) as ProcessInfo;
    hilog.info(DOMAIN, 'GameTest',
      'game launch path=%{public}s argc=%{public}d env=%{public}d backend=%{public}s perf=%{public}s '
        + 'pid=%{public}d status=%{public}s',
      gamePath, this.gameArguments.length, this.d3dEnvKeys.length,
      this.d3dBackend, this.perfProfileText,
      result.pid, result.state);
    if (this.experimentMode && this.experimentStaged &&
      this.smokeRequest.experimentResultName.length > 0) {
      this.monitorExperimentResult(this.smokeRequest.experimentId,
        this.smokeRequest.experimentResultName);
    }
    if (this.gameClickTitlePrefix.length > 0 && this.gameClickButtonText.length > 0) {
      const titlePrefix = this.gameClickTitlePrefix;
      const buttonText = this.gameClickButtonText;
      const delayMs = this.gameClickDelayMs;
      const clickXPermille = this.gameClickClientXPermille;
      const clickYPermille = this.gameClickClientYPermille;
      setTimeout(() => {
        const driverArguments = ['--attach', '--title-prefix', titlePrefix,
          '--button-text', buttonText, '--timeout-ms', '60000'];
        if (clickXPermille >= 0 && clickYPermille >= 0) {
          driverArguments.push('--client-x-permille', `${clickXPermille}`,
            '--client-y-permille', `${clickYPermille}`);
        }
        const driverResult = testNapi.runWineProgram({
          windowsExePath: 'C:\\smoke\\x64\\winehua_win32_driver.exe',
          argv: driverArguments,
          environment: {},
          workingDirectory: 'C:\\smoke\\x64',
          prefixMode: 'reuse',
          d3dBackend: this.d3dBackend,
          presentBackend: this.d3dPresentBackend(),
          automationMode: false
        }) as ProcessInfo;
        hilog.info(DOMAIN, 'GameTest',
          'click driver title=%{public}s button=%{public}s relative=%{public}d,%{public}d '
            + 'pid=%{public}d status=%{public}s',
          titlePrefix, buttonText, clickXPermille, clickYPermille,
          driverResult.pid, driverResult.state);
      }, delayMs);
    }
  }

  private monitorExperimentResult = async (experimentId: string, resultName: string): Promise<void> => {
    const resultsDir = `${this.driveC}/smoke/experiments/${experimentId}/results`;
    const path = `${resultsDir}/${resultName}.json`;
    const checkpointPath = `${resultsDir}/${resultName}.checkpoint.json`;
    let lastCheckpoint = '';
    for (let attempt = 0; attempt < 180; ++attempt) {
      const checkpoint = readTextIfExists(checkpointPath);
      if (checkpoint.length > 0 && checkpoint !== lastCheckpoint) {
        lastCheckpoint = checkpoint;
        hilog.info(DOMAIN, 'Experiment', 'checkpoint id=%{public}s path=%{public}s json=%{public}s',
          experimentId, checkpointPath, checkpoint);
      }
      if (pathExists(path)) {
        const result = readTextIfExists(path);
        this.setInitStatus(result.length > 0 ? '实验结果已生成' : '实验结果为空', false);
        hilog.info(DOMAIN, 'Experiment',
          'result id=%{public}s path=%{public}s json=%{public}s checkpoint=%{public}s',
          experimentId, path, result, lastCheckpoint);
        return;
      }
      await new Promise<void>(resolve => setTimeout(resolve, 1000));
    }
    this.setInitStatus('实验结果超时', false);
    hilog.error(DOMAIN, 'Experiment', 'result timeout id=%{public}s path=%{public}s', experimentId, path);
  }

  private startPendingVkd3dSmoke = (): void => {
    if (!this.pendingVkd3dSmoke || !this.prefixReady) return;
    this.pendingVkd3dSmoke = false;
    const exePath = 'C:\\smoke\\x64\\winehua_d3d12_smoke.exe';
    if (!stageManagedVkd3dRuntime('clean')) {
      this.statusText = 'DX12 示例启动失败（VKD3D DLL 校验失败）';
      hilog.error(DOMAIN, 'VKD3D', 'manual smoke runtime staging failed');
      return;
    }
    const result = testNapi.runWineProgram({
      windowsExePath: exePath,
      argv: ['--frames', '1000', '--width', '640', '--height', '480', '--sync-interval', '0'],
      environment: this.capabilityProbeEnvironment(exePath),
      workingDirectory: 'C:\\smoke\\x64',
      prefixMode: 'clean',
      d3dBackend: 'vkd3d_limited_500k',
      presentBackend: 'venus_broker_present',
      automationMode: true
    }) as ProcessInfo;
    this.statusText = result.pid > 0 ? 'DX12 示例运行中（1000 帧）' : 'DX12 示例启动失败';
    hilog.info(DOMAIN, 'VKD3D',
      'manual smoke path=%{public}s pid=%{public}d status=%{public}s',
      exePath, result.pid, result.state);
  }

  private startVkd3dSmoke = (): void => {
    if (this.initInProgress) return;
    this.selectD3dBackend('vkd3d_limited_500k');
    this.pendingVkd3dSmoke = true;
    // Run the opt-in probe in the same isolated, managed-window boundary as
    // the qualified Gate C experiments. This keeps the user's normal prefix
    // and desktop/DXVK session untouched by the test payload.
    this.automationMode = true;
    WineWindowManager.getInstance().setAutomationMode(true);
    this.prefixMode = 'clean';
    this.prefixRoot = WINE_SMOKE_PREFIX;
    this.driveC = `${this.prefixRoot}/drive_c`;
    this.sockPath = `${this.prefixRoot}/wine-wayland`;
    this.cleanPrefixPrepared = false;
    this.gameTestMode = true;
    this.statusText = '正在切换到隔离 VKD3D 测试会话...';
    this.initInProgress = true;
    this.initOverlayVisible = true;
    this.initOverlayText = '正在准备独立 VKD3D 测试环境（普通 Wine 前缀不变）';
    try {
      // A running VirGL host deliberately refuses live profile mutation. The
      // isolated prefix uses a different Wayland socket, so stop the current
      // server as well and let doInit() recreate the complete Gate C session.
      testNapi.setHostShadowProfile('shadow-precise-direct-fence');
      testNapi.stopAll();
      this.processList = [];
      this.state = 'idle';
      this.prefixReady = false;
      this.initInProgress = false;
      setTimeout(() => this.doInit(), 250);
    } catch (e) {
      this.pendingVkd3dSmoke = false;
      this.initInProgress = false;
      this.setInitStatus('VKD3D 测试会话切换失败', false);
      hilog.error(DOMAIN, 'VKD3D', 'manual session restart failed: %{public}s', JSON.stringify(e));
    }
  }

  private startDeveloperSmoke = (): void => {
    this.smokeRequest = {
      mode: 'smoke', runId: `developer-${Date.now()}`, suite: 'core',
      prefixMode: 'reuse', longSeconds: 3600, batchMappedFlush: false,
      experimentId: '', experimentExecutable: '', experimentResultName: '', experimentSourceUrl: ''
    };
    this.automationMode = true;
    this.gameTestMode = false;
    this.prefixMode = 'reuse';
    this.prefixRoot = `${FILES_BASE}/.wine`;
    this.driveC = `${this.prefixRoot}/drive_c`;
    this.sockPath = `${this.prefixRoot}/wine-wayland`;
    this.smokeStarted = false;
    if (this.prefixReady) this.startRequestedSmoke();
    else this.doInit();
  }

  build() {
    Stack() {
    Row() {
      // ============================================================
      //  侧边栏
      // ============================================================
      Column() {
        // -- Wine 环境状态 --
        Text('Wine 环境')
          .fontWeight(FontWeight.Bold).fontColor('#999')
          .width('100%').padding({ top: 12, bottom: 6 })

        Row() {
          Circle({ width: 8, height: 8 })
            .fill(this.prefixReady ? '#4f4' : '#f84')
            .margin({ right: 6 })
          Text(this.statusText)
            .fontColor(this.state == 'wine-running' ? '#4f4' :
                       this.state == 'exited' ? '#f84' :
                       this.prefixReady ? '#8cf' : '#888')
            .layoutWeight(1)
            .maxLines(2).textOverflow({ overflow: TextOverflow.Ellipsis })
        }
        .width('100%').padding({ bottom: 12 })
        .alignItems(VerticalAlign.Center)

        Divider().color('#2a2a2a').width('100%')

        Text('D3D backend')
          .fontWeight(FontWeight.Bold).fontColor('#999')
          .width('100%').padding({ top: 12, bottom: 8 })

        Row({ space: 4 }) {
          Button('WineD3D')
            .layoutWeight(1).height(34)
            .backgroundColor(this.d3dBackend === 'wined3d' ? '#285' : '#333')
            .onClick(() => this.selectD3dBackend('wined3d'))
          Button('DXVK Legacy')
            .layoutWeight(1).height(34)
            .backgroundColor(this.d3dBackend === 'dxvk_legacy' ? '#285' : '#333')
            .onClick(() => this.selectD3dBackend('dxvk_legacy'))
        }
        .width('100%').margin({ bottom: 8 })

        Button('VKD3D 2.6（500K 实验）')
          .width('100%').height(34)
          .backgroundColor(this.d3dBackend === 'vkd3d_limited_500k' ? '#854' : '#333')
          .onClick(() => this.selectD3dBackend('vkd3d_limited_500k'))
          .margin({ bottom: 6 })

        if (this.d3dBackend === 'vkd3d_limited_500k') {
          Text('仅 x64；资源描述符堆上限 500,000；查询 Meta 暂不可用。普通启动仍使用 DXVK Legacy。')
            .fontSize(11).fontColor('#d9a').maxLines(3)
            .width('100%').margin({ bottom: 6 })
          Button('运行 DX12 1000 帧示例')
            .width('100%').height(36).backgroundColor('#764')
            .enabled(!this.initInProgress)
            .onClick(this.startVkd3dSmoke)
            .margin({ bottom: 8 })
        }

        // -- 快速操作 --
        Text('快速操作')
          .fontWeight(FontWeight.Bold).fontColor('#999')
          .width('100%').padding({ top: 12, bottom: 8 })

        if (!this.prefixReady) {
          Button('初始化 Wine')
            .width('100%').backgroundColor('#285')
            .enabled(!this.initInProgress)
            .onClick(this.doInit)
            .margin({ bottom: 8 })
        } else {
          Button('重置 Wine')
            .width('100%').backgroundColor('#633')
            .enabled(!this.initInProgress && this.state != 'wine-running')
            .onClick(this.doReset)
            .margin({ bottom: 8 })
          Button('重启 Wine')
            .width('100%').backgroundColor('#652')
            .enabled(!this.initInProgress && this.state != 'wine-running')
            .onClick(this.doInit)
            .margin({ bottom: 8 })
        }

        Button('开发测试（Core）')
          .width('100%').backgroundColor('#246')
          .enabled(!this.initInProgress)
          .onClick(this.startDeveloperSmoke)
          .margin({ bottom: 8 })

        Divider().color('#2a2a2a').width('100%')

        // -- 运行中进程 --
        Row() {
          Text('运行中进程')
            .fontWeight(FontWeight.Bold).fontColor('#999')
            .layoutWeight(1)
          Button({ type: ButtonType.Circle, stateEffect: true }) {
            Text('↻')
          }
          .width(28).height(28).backgroundColor('#333')
          .onClick(this.refreshProcessList)
        }
        .width('100%').padding({ top: 12, bottom: 8 })
        .alignItems(VerticalAlign.Center)

        if (this.processList.length > 0) {
          List({ space: 1 }) {
            ForEach(this.processList, (proc: ProcessInfo) => {
              ListItem() {
                Row() {
                  Column() {
                    Text(proc.name)
                      .fontColor('#dfd')
                      .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                  }
                  .layoutWeight(1).alignItems(HorizontalAlign.Start)

                  if (proc.state === 'running') {
                    Circle({ width: 6, height: 6 }).fill('#4f4').margin({ right: 10 })
                  }
                  Text('✕')
                    .fontColor('#f55')
                    .onClick(() => this.doKillProcess(proc.pid))
                }
                .width('100%').padding({ left: 14, right: 14, top: 10, bottom: 10 })
                .alignItems(VerticalAlign.Center)
              }
              .backgroundColor('#1e1e1e')
            })
          }
          .width('100%').layoutWeight(1)
          .margin({ left: -14, right: -14 })
          .scrollBar(BarState.Off)
        } else {
          Text(this.prefixReady ? '无运行中进程' : 'Wine 未初始化')
            .fontColor('#555')
            .width('100%').padding({ top: 8 })
        }
      }
      .width(300).height('100%')
      .backgroundColor('#141414')
      .padding({ left: 14, right: 14, top: 10, bottom: 10 })

      // -- 侧边栏分隔线 --
      Divider().vertical(true).color('#2a2a2a').height('100%')

      // ============================================================
      //  右侧主内容区
      // ============================================================
      Column() {
        // -- 标题栏 --
        Row() {
          Text($r('app.string.app_name'))
            .fontWeight(FontWeight.Bold).fontColor('#fff')

          Text(this.statusText)
            .fontColor(this.state == 'wine-running' ? '#4f4' :
                       this.state == 'exited' ? '#f84' :
                       this.prefixReady ? '#8cf' : '#888')
            .margin({ left: 12 }).layoutWeight(1)
            .maxLines(1)
        }
        .width('100%')
        .padding({ left: 14, right: 14, top: 10, bottom: 10 })
        .backgroundColor('#1a1a1a')
        .alignItems(VerticalAlign.Center)

        // -- 文件浏览器 --
        if (this.prefixReady) {
          Row() {
            if (this.curDir != this.driveC) {
              Button('← 上级目录')
                .backgroundColor('#333')
                .onClick(this.goUp)
            }
            Text(this.relPath())
              .fontColor('#888')
              .margin({ left: 10 }).layoutWeight(1)
              .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
          }
          .width('100%').padding({ left: 14, right: 14, top: 10, bottom: 6 })

          List({ space: 1 }) {
            ForEach(this.entries, (name: string) => {
              ListItem() {
                Row() {
                  Text(this.isDir(this.curDir + '/' + name) ? '📁' : (this.isExe(name) ? '▶' : '  '))
                    .width(28)
                  Text(name)
                    .fontColor(this.isExe(name) ? '#dfd' :
                               this.isDir(this.curDir + '/' + name) ? '#8cf' : '#888')
                    .layoutWeight(1)
                    .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                  if (this.isExe(name)) {
                    Button('启动')
                      .height(32)
                      .enabled(!this.initInProgress)
                      .onClick(() => this.doRun(name))
                  }
                }
                .width('100%').height(44).padding({ left: 14, right: 14 })
                .alignItems(VerticalAlign.Center)
                .onClick(() => {
                  if (this.isDir(this.curDir + '/' + name)) {
                    this.enterDir(name);
                  }
                })
              }
              .backgroundColor('#1e1e1e')
            })
          }
          .width('100%').layoutWeight(1)
        } else {
          Column() {
            Text('需要先初始化 Wine 环境')
              .fontColor('#555')
          }
          .width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
        }
      }
      .layoutWeight(1).height('100%')
      .backgroundColor('#111')
    }
    .width('100%').height('100%')
    if (this.initOverlayVisible && !this.prefixReady) {
      Column() {
        LoadingProgress()
          .width(42)
          .height(42)
          .color('#7db8ff')
          .margin({ bottom: 18 })
        Text('正在准备 Wine')
          .fontSize(20)
          .fontWeight(FontWeight.Medium)
          .fontColor('#f4f7fb')
          .margin({ bottom: 8 })
        Text(this.initOverlayText)
          .fontSize(15)
          .fontColor('#aeb8c6')
          .maxLines(2)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .width(360)
      .padding({ left: 28, right: 28, top: 26, bottom: 26 })
      .backgroundColor('#20242b')
      .borderRadius(8)
      .alignItems(HorizontalAlign.Center)
      .shadow({ radius: 18, color: '#66000000', offsetX: 0, offsetY: 8 })
    }
    }
    .width('100%').height('100%')
  }
}
