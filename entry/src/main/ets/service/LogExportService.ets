// 日志导出服务 (环境页"会话维护"卡): 打包沙箱日志到 Download 应用目录, 供问题反馈。
// 原 WineEnvService.exportLogs 拆分独立 (发布版实用功能, 不属于核心引擎 —
// master 核心版 WineEnvService 不含本服务)。状态头字段来自 WineEnvService (入参),
// 避免与引擎服务产生双向依赖。
import { hilog } from '@kit.PerformanceAnalysisKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { ensureDirectory, writeTextFile } from './WineEnvService';
import { WineEnvService } from './WineEnvService';

const DOMAIN = 0x0000;

export class LogExportService {
  private static instance: LogExportService | null = null;

  static getInstance(): LogExportService {
    if (!LogExportService.instance) {
      LogExportService.instance = new LogExportService();
    }
    return LogExportService.instance;
  }

  // 汇总 状态头 + temp/cache 下的日志文件 (总量封顶 512KB, 单文件 128KB 截断),
  // 返回输出路径 ('' = 失败, UI toast 提示)
  exportLogs(env: WineEnvService): string {
    const CAP_TOTAL = 512 * 1024;
    const CAP_FILE = 128 * 1024;
    let bundle = `WineHua 日志导出 ${new Date().toLocaleString()}\n` +
      `engineState=${env.engineState} stage=${env.engineStage} prefixReady=${env.prefixReady} status=${env.statusText}\n` +
      `d3dBackend=${env.getD3dBackend()} perfProfile=${env.getPerfProfile()} zHome=${env.zHome}\n\n`;
    let total = bundle.length;
    const dirs = ['/data/storage/el2/base/temp', '/data/storage/el2/base/cache'];
    for (const dir of dirs) {
      let names: string[] = [];
      try {
        names = fs.listFileSync(dir);
      } catch (_) { continue; }
      for (const name of names) {
        // 只收文本日志, 跳过二进制/目录
        if (!/\.(log|txt)$/i.test(name)) continue;
        const path = `${dir}/${name}`;
        let text = '';
        try {
          const st = fs.statSync(path);
          if (!st.isFile()) continue;
          const file = fs.openSync(path, fs.OpenMode.READ_ONLY);
          try {
            const size = Math.min(Number(st.size), CAP_FILE);
            const buf = new ArrayBuffer(size);
            const count = fs.readSync(file.fd, buf);
            const bytes = new Uint8Array(buf, 0, count);
            let s = '';
            for (let i = 0; i < bytes.length; ++i) s += String.fromCharCode(bytes[i]);
            text = s;
          } finally {
            fs.closeSync(file);
          }
        } catch (_) { continue; }
        if (total + text.length > CAP_TOTAL) {
          bundle += `---- ${path} (跳过: 超出总量上限) ----\n\n`;
          continue;
        }
        bundle += `---- ${path} ----\n${text}\n\n`;
        total += text.length;
      }
    }
    try {
      const outDir = '/storage/Users/currentUser/Download/app.hackeris.winehua';
      ensureDirectory(outDir);
      const ts = new Date().getTime();
      const outPath = `${outDir}/winehua-logs-${ts}.log`;
      writeTextFile(outPath, bundle);
      hilog.info(DOMAIN, 'wine', 'exportLogs → %{public}s (%{public}d bytes)', outPath, total);
      return outPath;
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'exportLogs fail: %{public}s', JSON.stringify(e));
      return '';
    }
  }
}
