// Windows 字体导入服务 (设置页"Windows 字体"入口): 用户把 C:\Windows\Fonts 打包为
// zip, 经 picker 选中后解压、把字体文件平铺拷入 prefix 的 windows/Fonts。
// 原 WineEnvService.importFontsZip 拆分独立 (发布版实用功能, 不属于核心引擎 —
// master 核心版 WineEnvService 不含本服务)。
import { hilog } from '@kit.PerformanceAnalysisKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { DRIVE_C, pathExists, ensureDirectory, removePathRecursive } from './WineEnvService';

const DOMAIN = 0x0000;

// 字体导入校验: 常见 Windows 系统字体文件名 (小写 basename)。zip 内容必须命中
// 至少一个才认可是 C:\Windows\Fonts 的备份 — 覆盖西文核心 (Win7~11) 与
// 中日韩 Windows 常见字体, 防用户选错 zip 把杂文件灌进 Fonts 目录
const KNOWN_CORE_FONTS = new Set<string>([
  // 西文核心
  'arial.ttf', 'arialbd.ttf', 'times.ttf', 'timesbd.ttf', 'cour.ttf', 'courbd.ttf',
  'tahoma.ttf', 'verdana.ttf', 'georgia.ttf', 'trebuc.ttf', 'impact.ttf', 'comic.ttf',
  'calibri.ttf', 'cambria.ttc', 'candara.ttf', 'consola.ttf', 'constan.ttf', 'corbel.ttf',
  'segoeui.ttf', 'symbol.ttf', 'webdings.ttf', 'wingding.ttf', 'marlett.ttf', 'micross.ttf',
  // 中文 (简体/繁体)
  'simsun.ttc', 'simsunb.ttf', 'simsunextg.ttf', 'simhei.ttf', 'simfang.ttf', 'simkai.ttf',
  'msyh.ttc', 'msyhbd.ttc', 'deng.ttf', 'mingliu.ttc', 'mingliub.ttc', 'msjh.ttc', 'msjhbd.ttc',
  // 日韩
  'msgothic.ttc', 'malgun.ttf', 'gulim.ttc', 'batang.ttc'
]);

export class FontImportService {
  private static instance: FontImportService | null = null;

  static getInstance(): FontImportService {
    if (!FontImportService.instance) {
      FontImportService.instance = new FontImportService();
    }
    return FontImportService.instance;
  }

  // 导入进行中: 设置页按钮使能 + Index 根 Stack 全窗遮罩镜像
  // (与 AppLibraryService.scanning 同一方案, 解压大字体包可达数十秒)
  fontImporting: boolean = false;

  // 极简订阅 (与 WineEnvService 同款): 状态变化后逐个调用
  private listeners: Array<() => void> = [];

  subscribe(listener: () => void): void {
    if (!this.listeners.includes(listener)) {
      this.listeners.push(listener);
    }
  }

  private notify(): void {
    for (const listener of this.listeners) {
      listener();
    }
  }

  // Windows 字体导入 (设置页): 用户把 C:\Windows\Fonts 打包为 zip, 经 picker 选中后
  // 解压、把字体文件平铺拷入 prefix 的 windows/Fonts。zip 内目录结构不保证 —
  // 有的带 Fonts/ 顶层目录、有的直接是字体文件, 故不按结构解压到目标, 而是
  // 递归收集字体文件逐个拷贝 (Windows 的 Fonts 目录本身就是平铺的)。
  // 同名覆盖 (与 Windows 更新字体的语义一致)。导入前做三段内容校验
  // (扩展名 → 魔数 → 常见系统字体名命中), 任一不过整包拒绝。
  // 返回结果文案 (UI toast); 全程 fontImporting 置位, Index 全窗遮罩阻塞操作。
  async importFontsZip(uri: string): Promise<string> {
    const fontsDir = `${DRIVE_C}/windows/Fonts`;
    if (!pathExists(DRIVE_C)) {
      return 'Wine 环境未初始化，请先初始化';
    }
    this.fontImporting = true;
    this.notify();
    // 中转目录放 cache (可系统回收); 先清掉上次异常残留
    const tmpRoot = '/data/storage/el2/base/cache/font-import';
    try {
      if (pathExists(tmpRoot)) {
        removePathRecursive(tmpRoot);
      }
      ensureDirectory(tmpRoot);
      const zipPath = `${tmpRoot}/fonts.zip`;
      const extractDir = `${tmpRoot}/x`;
      ensureDirectory(extractDir);
      // picker 授权的是 URI, openSync 直接吃 URI; 中转走沙箱路径
      // (decompressFile 只吃沙箱路径)
      let src: fs.File | null = null;
      let dst: fs.File | null = null;
      try {
        src = fs.openSync(uri, fs.OpenMode.READ_ONLY);
        dst = fs.openSync(zipPath,
          fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
        fs.copyFileSync(src.fd, dst.fd);
      } finally {
        if (src) {
          fs.closeSync(src);
        }
        if (dst) {
          fs.closeSync(dst);
        }
      }
      await zlib.decompressFile(zipPath, extractDir, {});
      if (!ensureDirectory(fontsDir)) {
        return '字体目录创建失败';
      }
      // 三段校验: ① 扩展名收集 → ② 魔数校验文件内容 (防改名假字体) →
      // ③ 常见系统字体名命中 (确认是 C:\Windows\Fonts 的备份, 而非任意字体包)。
      // 任一不过则整包拒绝, 不往 Fonts 目录写任何文件
      const all: string[] = [];
      this.collectFontFiles(extractDir, all);
      if (all.length == 0) {
        return '压缩包中未找到字体文件';
      }
      const valid: string[] = [];
      for (const p of all) {
        if (this.checkFontMagic(p)) {
          valid.push(p);
        }
      }
      if (valid.length == 0) {
        return '字体文件校验未通过（内容不是有效字体）';
      }
      let knownHits = 0;
      for (const p of valid) {
        if (KNOWN_CORE_FONTS.has(p.substring(p.lastIndexOf('/') + 1).toLowerCase())) {
          knownHits++;
        }
      }
      if (knownHits == 0) {
        return '内容不像 Windows 的 Fonts 目录（缺少常见系统字体），已取消导入';
      }
      const copied = this.copyFontFiles(valid, fontsDir);
      if (copied == 0) {
        return '字体拷贝失败';
      }
      hilog.info(DOMAIN, 'wine', 'importFontsZip: %{public}d/%{public}d fonts → %{public}s',
        copied, valid.length, fontsDir);
      let msg = `已导入 ${copied} 个字体文件`;
      if (all.length > valid.length) {
        msg += `，跳过 ${all.length - valid.length} 个无效文件`;
      }
      return msg;
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'importFontsZip fail: %{public}s', JSON.stringify(e));
      return '字体导入失败，请确认 zip 包完整';
    } finally {
      try {
        if (pathExists(tmpRoot)) {
          removePathRecursive(tmpRoot);
        }
      } catch (_) { /* 清理失败无碍, 下次导入前还会再清 */ }
      this.fontImporting = false;
      this.notify();
    }
  }

  // 递归收集 dir 下的字体扩展名文件路径 (仅按名字初筛, 内容校验在 checkFontMagic)
  private collectFontFiles(dir: string, out: string[]): void {
    let names: string[] = [];
    try {
      names = fs.listFileSync(dir);
    } catch (_) {
      return;
    }
    for (const name of names) {
      const path = `${dir}/${name}`;
      let st: fs.Stat;
      try {
        st = fs.statSync(path);
      } catch (_) {
        continue;
      }
      if (st.isDirectory()) {
        this.collectFontFiles(path, out);
        continue;
      }
      if (/\.(ttf|ttc|otf|fon|fnt)$/i.test(name)) {
        out.push(path);
      }
    }
  }

  // 字体文件内容魔数校验: ttf = 0x00010000 或 'true', ttc = 'ttcf', otf = 'OTTO',
  // fon = 'MZ' (字体资源 DLL, NE/PE 头); fnt 是原始位图无可靠魔数, 直接放行
  private checkFontMagic(path: string): boolean {
    const lower = path.toLowerCase();
    if (lower.endsWith('.fnt')) {
      return true;
    }
    let file: fs.File | null = null;
    try {
      file = fs.openSync(path, fs.OpenMode.READ_ONLY);
      const buf = new ArrayBuffer(4);
      if (fs.readSync(file.fd, buf) < 4) {
        return false;
      }
      const b = new Uint8Array(buf);
      if (lower.endsWith('.ttf')) {
        return (b[0] == 0 && b[1] == 1 && b[2] == 0 && b[3] == 0) ||
          (b[0] == 0x74 && b[1] == 0x72 && b[2] == 0x75 && b[3] == 0x65);
      }
      if (lower.endsWith('.ttc')) {
        return b[0] == 0x74 && b[1] == 0x74 && b[2] == 0x63 && b[3] == 0x66;
      }
      if (lower.endsWith('.otf')) {
        return b[0] == 0x4F && b[1] == 0x54 && b[2] == 0x54 && b[3] == 0x4F;
      }
      if (lower.endsWith('.fon')) {
        return b[0] == 0x4D && b[1] == 0x5A;
      }
      return false;
    } catch (_) {
      return false;
    } finally {
      if (file) {
        fs.closeSync(file);
      }
    }
  }

  // 把校验过的字体文件按 basename 平铺拷入 fontsDir, 返回拷贝数; 单个失败跳过
  private copyFontFiles(files: string[], fontsDir: string): number {
    let copied = 0;
    for (const path of files) {
      const name = path.substring(path.lastIndexOf('/') + 1);
      let src: fs.File | null = null;
      let dst: fs.File | null = null;
      try {
        src = fs.openSync(path, fs.OpenMode.READ_ONLY);
        dst = fs.openSync(`${fontsDir}/${name}`,
          fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
        fs.copyFileSync(src.fd, dst.fd);
        copied++;
      } catch (e) {
        hilog.warn(DOMAIN, 'wine', 'font copy fail %{public}s: %{public}s', name, JSON.stringify(e));
      } finally {
        if (src) {
          fs.closeSync(src);
        }
        if (dst) {
          fs.closeSync(dst);
        }
      }
    }
    return copied;
  }
}
