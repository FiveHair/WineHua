#include "wine_env.h"
#include "wine_constants.h"
#include "audio_broker.h"
#include "audio_ipc_protocol.h"
#include "graphics_broker.h"
#include "wayland_server.h"

#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_set>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

int CreateAudioBootstrapFd(const std::string& runtimeDir) {
    if (!winehua::AudioBroker::GetInstance().EnsureStarted(runtimeDir)) {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to start for runtimeDir=%{public}s", runtimeDir.c_str());
        return -1;
    }
    int fd = winehua::AudioBroker::GetInstance().CreateBootstrapHandle();
    if (fd < 0) {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to create bootstrap FD for runtimeDir=%{public}s", runtimeDir.c_str());
        return -1;
    }
    OH_LOG_INFO(LOG_APP, "[AudioBroker] bootstrap ready runtimeDir=%{public}s", runtimeDir.c_str());
    return fd;
}

std::vector<std::string> BuildWineEnv(const std::string& sockDir,
                                      const std::string& sockName,
                                      const std::string& libPath,
                                      const std::string& binDir,
                                      int audioBootstrapFd,
                                      const std::string& homeDir,
                                      const std::string& prefixDir,
                                      const std::string& wineLang) {
    std::string shareDir = binDir + "/../share";
    std::string xkbDir = shareDir + "/X11/xkb";
    std::string midiSoundfontPath = binDir + "/../audio/winehua-gm.sf2";
    std::string runtimeLibPath = binDir + ":" + binDir + "/x86_64-unix:" + binDir + "/../lib/x86_64";
    winehua::GraphicsBackendState graphicsState = winehua::GraphicsBroker::GetInstance().GetState();
    std::string guestReceiverLibDir;
    bool useGuestReceiverRuntime = graphicsState.active == winehua::GraphicsBackend::Virgl;

    if (useGuestReceiverRuntime && graphicsState.guestReceiverPresent && !graphicsState.guestReceiverRuntimeDir.empty()) {
        guestReceiverLibDir = graphicsState.guestReceiverRuntimeDir + "/lib";
        if (access(guestReceiverLibDir.c_str(), F_OK) == 0) {
            runtimeLibPath = guestReceiverLibDir + ":" + runtimeLibPath;
        }
    }

    std::string dllPath = binDir + "/x86_64-windows:" + binDir + "/i386-windows:" + binDir;
#ifndef __aarch64__
    // x86_64: bundled libs 加入 WINEDLLPATH, load_unixlib_by_name() 从此搜索 .so
    dllPath += ":/data/storage/el1/bundle/libs/x86_64";
#endif

    // ==== Layer 0: 硬基线 (路径、locale、Wayland socket) ====
    // NOTE: WINEDLLDIR0/1, WINEDLLPATH 在 DXVK 路径下会被 AppendD3dBackendEnv 覆盖
    std::vector<std::string> env = {
        "XDG_RUNTIME_DIR=" + sockDir,
        "WAYLAND_DISPLAY=" + sockName,
        "HOME=" + homeDir,
        "WINEPREFIX=" + (prefixDir.empty() ? std::string(WINE_PREFIX) : prefixDir),
        "WINEDATADIR=" + shareDir + "/wine",
        "WINEDLLDIR=" + binDir + "/x86_64-unix",
        "WINEDLLDIR0=" + binDir + "/x86_64-windows",
        "WINEDLLDIR1=" + binDir + "/i386-windows",
        "WINEDLLDIR2=" + binDir,
        "WINEDLLPATH=" + dllPath,
        "WINEDEBUG=-all",
        "LANG=" + wineLang + ".UTF-8",
        // OHOS musl 无 locale 数据, setlocale 激活失败返回 "C";
        // Wine 的 unix_to_win_locale 遇 "C" 只读 LC_ALL 兜底 (ntdll/unix/env.c),
        // 单设 LANG 无效, 必须补 LC_ALL 才能解析出对应 LCID (0x0804 zh-CN),
        // 与 LANG 同取设置页 wineLang (zh_CN/en_US)
        "LC_ALL=" + wineLang + ".UTF-8",
        "XKB_CONFIG_ROOT=" + xkbDir,
        "PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:" + binDir + "/x86_64-windows:" + binDir + "/i386-windows:" + binDir,
        "TMPDIR=" WINE_TMPDIR,
        "MIDI_SOUNDFONT_PATH=" + midiSoundfontPath,
        // winegstreamer 运行时加载 GStreamer 插件 (gst-plugins-base/good/libav)
        "GST_PLUGIN_PATH=" + binDir + "/x86_64-unix/gstreamer-1.0",
        "GST_PLUGIN_SYSTEM_PATH=" + binDir + "/x86_64-unix/gstreamer-1.0",
    };
    // ==== Layer 1: Box64 性能调优 (仅 ARM64) ====
    // NOTE: BOX64_DYNAREC_WEAKBARRIER=2 在桌面 DXVK 下会被 AppendStableDesktopDxvkEnv 覆盖为 0
    AppendBox64PerfStrings(env);
    // ==== Layer 2: 运行时库路径 ====
    // NOTE: BOX64_LD_LIBRARY_PATH (ARM64) 在 DXVK 路径下会被 AppendD3dBackendEnv 覆盖
#ifdef __aarch64__
    env.push_back("LD_LIBRARY_PATH=" + libPath);
    env.push_back("BOX64_LD_LIBRARY_PATH=" + runtimeLibPath);
#else
    env.push_back("LD_LIBRARY_PATH=" + runtimeLibPath);
#endif
    // ==== Layer 3: 音频 bootstrap (条件) ====
    if (audioBootstrapFd >= 0) {
        env.push_back("WINE_OHOS_AUDIO_ENABLE=1");
        env.push_back("WINE_OHOS_AUDIO_BOOTSTRAP_FD=" + std::to_string(audioBootstrapFd));
        env.push_back("WINE_OHOS_AUDIO_PROTOCOL_VERSION=" + std::to_string(WINEHUA_AUDIO_PROTOCOL_VERSION));
    }
    // ==== Layer 4: 桌面模式标记 ====
    winehua::GraphicsBroker::GetInstance().SetWineRuntimeBinaryDir(binDir);
    // 告知 winewayland.drv 当前是桌面模式还是独立窗口模式
    // NOTE: 桌面模式下 wine_child.cpp 也会通过 __winehua_desktop__ token 设置同值（冗余保险）
    env.push_back(std::string("WINEHUA_DESKTOP_MODE=") +
                  (WaylandServer::GetInstance()->IsDesktopMode() ? "1" : "0"));
    // WINEHUA_SIMULATE_RESOLUTION: win32u per-process 模拟 ChangeDisplaySettings
    // (记录游戏主动 CDS 请求的分辨率, 查询时返回 — DDraw 全屏游戏依赖)。
    // 仅 PC 多窗口模式注入: Pad 模拟桌面 (RootCompositing) 由合成器缩放绘制,
    // 不需要分辨率模拟。
    if (!WaylandServer::GetInstance()->IsDesktopMode())
        env.push_back("WINEHUA_SIMULATE_RESOLUTION=1");
    // 相对模式 enter 静默校准 (方向 A) 启用开关: wine 侧只在显式 "1" 时启用。
    // 默认注入 "1" (静默校准 = 默认行为, 三游戏验证通过); 排查问题时改 "0"
    // (或删除本行 = 未设置) 回退硬件绝对移动路径。
    env.push_back("WINEWAYLAND_ENTER_SILENT=1");
    // ==== Layer 5: 图形状态 ====
    // NOTE: BOX64_EMULATED_LIBS (ARM64) 在 DXVK 路径下会被 AppendD3dBackendEnv 覆盖
    winehua::GraphicsBroker::GetInstance().AppendWineEnv(env);

    OH_LOG_INFO(LOG_APP,
                "[WineEnv] backend=%{public}s guestMode=%{public}s guestLib=%{public}s runtimeLibPath=%{public}s",
                winehua::GraphicsBroker::BackendName(graphicsState.active),
                graphicsState.guestReceiverMode.empty() ? "stock-egl" : graphicsState.guestReceiverMode.c_str(),
                guestReceiverLibDir.empty() ? "(none)" : guestReceiverLibDir.c_str(),
                runtimeLibPath.c_str());
    return env;
}

void UpsertEnvLine(std::vector<std::string>& env, const std::string& line)
{
    const size_t sep = line.find('=');
    if (sep == std::string::npos || sep == 0) return;
    const std::string key = line.substr(0, sep);
    // 清理所有同 key 的旧条目, 然后追加新值 (与旧 UpsertEnv 行为一致,
    // 避免 AppendStableDesktopDxvkEnv 等 push_back 路径产生重复 key)
    env.erase(std::remove_if(env.begin(), env.end(), [&](const std::string& existing) {
        return existing.compare(0, key.size(), key) == 0 &&
               existing.size() > key.size() && existing[key.size()] == '=';
    }), env.end());
    env.push_back(line);
}

void AppendD3dBackendEnv(std::vector<std::string>& env,
                         const std::string& d3dBackend,
                         const std::string& dxvkBackend,
                         const std::string& binDir)
{
    if (d3dBackend == "vkd3d_limited_500k")
    {
        const bool modern26 = dxvkBackend == "dxvk_modern_2_6";
        const std::string dxvkRuntimeProfile = modern26 ? "modern-2.6" : "legacy";
        const std::string overlayRoot = std::string(WINE_RUNTIME_ROOT) +
            "/vkd3d/limited-500k";
        const std::string overlay64 = overlayRoot + "/x64";
        const std::string dxvkRoot = std::string(WINE_RUNTIME_ROOT) +
            "/dxvk/" + dxvkRuntimeProfile;
        const std::string dxvk64 = dxvkRoot + "/x64";
        const std::string dxvk86 = dxvkRoot + "/x86";
        const std::string guestVulkanRoot = binDir + "/guest_vulkan";
        const std::string guestVulkanLib = guestVulkanRoot + "/lib";
        const std::string guestVulkanIcd = guestVulkanRoot +
            "/share/vulkan/icd.d/venus_icd.x86_64.json";
        const std::string box64LibraryPath = guestVulkanLib + ":" +
            binDir + "/guest_gfx/lib:" + binDir + ":" +
            binDir + "/x86_64-unix:" + std::string(WINE_RUNTIME_ROOT) +
            "/lib/x86_64";
        /* Keep VKD3D first for d3d12, then the independently selected DXVK
         * overlays for d3d11/dxgi. The Wine loader gives both overlay
         * families priority over an application's private DLL directory. */
        const std::string wineDllPath = overlay64 + ":" + dxvk64 + ":" +
            dxvk86 + ":" + binDir + "/x86_64-windows:" +
            binDir + "/i386-windows:" + binDir;
        const std::vector<std::string> managed = {
            "WINEHUA_D3D_BACKEND=" + d3dBackend,
            "WINEHUA_VKD3D_ROOT=" + overlayRoot,
            "WINEHUA_VKD3D_PROFILE=limited-500k",
            "WINEHUA_VKD3D_VERSION=2.6",
            "WINEHUA_DXVK_ROOT=" + dxvkRoot,
            "WINEHUA_DXVK_PROFILE=" + dxvkRuntimeProfile,
            "WINEHUA_DXVK_VERSION=" + std::string(modern26 ? "2.6.2" : "1.10.3"),
            /* Product sessions use the qualified precise mapping contract
             * without enabling the Gate C trace selector. Direct fence waits
             * remain enabled explicitly below. */
            "WINEHUA_PERF_PROFILE=shadow-precise",
            "WINEHUA_VULKAN_RUNTIME=1",
            "WINEHUA_VULKAN_LOADER_ARCH=x86_64",
            "WINEHUA_VENUS_ICD_ARCH=x86_64",
#ifdef __aarch64__
            "USE_LIBBOX64=1",
            "BOX64_LD_LIBRARY_PATH=" + box64LibraryPath,
            "BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:"
                "libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:"
                "libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:"
                "libwayland-client.so:libwayland-client.so.0:libwayland-server.so:"
                "libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:"
                "libdrm.so:libdrm.so.2:libffi.so:libffi.so.8:"
                "libglib-2.0.so:libglib-2.0.so.0:"
                "libgobject-2.0.so:libgobject-2.0.so.0:"
                "libgio-2.0.so:libgio-2.0.so.0:"
                "libgmodule-2.0.so:libgmodule-2.0.so.0:"
                "libgstreamer-1.0.so:libgstreamer-1.0.so.0:"
                "libgstbase-1.0.so:libgstbase-1.0.so.0:"
                "libgstvideo-1.0.so:libgstvideo-1.0.so.0:"
                "libgstaudio-1.0.so:libgstaudio-1.0.so.0:"
                "libgsttag-1.0.so:libgsttag-1.0.so.0:"
                "libgstpbutils-1.0.so:libgstpbutils-1.0.so.0:"
                "libgstallocators-1.0.so:libgstallocators-1.0.so.0:"
                "libgstapp-1.0.so:libgstapp-1.0.so.0:"
                "libgstcontroller-1.0.so:libgstcontroller-1.0.so.0:"
                "libgstfft-1.0.so:libgstfft-1.0.so.0:"
                "libgstnet-1.0.so:libgstnet-1.0.so.0:"
                "libgstriff-1.0.so:libgstriff-1.0.so.0:"
                "libgstrtp-1.0.so:libgstrtp-1.0.so.0:"
                "libgstrtsp-1.0.so:libgstrtsp-1.0.so.0:"
                "libgstsdp-1.0.so:libgstsdp-1.0.so.0:"
                "libgstcodecparsers-1.0.so:libgstcodecparsers-1.0.so.0:"
                "libgstmpegts-1.0.so:libgstmpegts-1.0.so.0:"
                "libxml2.so:libxml2.so.2:libz.so:libz.so.1",
            "BOX64_DYNAREC_WEAKBARRIER=0",
#endif
            "VK_DRIVER_FILES=" + guestVulkanIcd,
            "VK_ICD_FILENAMES=" + guestVulkanIcd,
            "VN_DEBUG=vtest",
            "VN_PERF=" + std::string(modern26
                ? "no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring"
                : "no_fence_feedback,no_query_feedback,no_multi_ring"),
            "VN_WINEHUA_STRONG_RING_BARRIER=1",
            "VN_WINEHUA_REMOTE_MEMORY_SYNC=1",
            "VN_WINEHUA_PERSISTENT_MAP_SYNC=1",
            "VN_WINEHUA_DIRECT_FENCE_WAIT=1",
            "VKR_WINEHUA_SHADOW_FROM_HOST=precise",
            "VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC=1",
            "WINEDLLOVERRIDES=d3d12=n;d3d11=n;dxgi=n",
            "WINEDLLPATH=" + wineDllPath,
            "WINEDLLDIR0=" + overlay64,
            "WINEDLLDIR1=" + dxvk64,
            "WINEDLLDIR2=" + dxvk86,
            /* ntdll stops scanning WINEDLLDIRn at the first missing index.
             * Keep Wine's PE runtime directories contiguous after the D3D
             * overlays so their imports can still resolve system DLLs. */
            "WINEDLLDIR3=" + binDir + "/x86_64-windows",
            "WINEDLLDIR4=" + binDir + "/i386-windows",
            "WINEDLLDIR5=" + binDir,
        };
        for (const std::string& line : managed) UpsertEnvLine(env, line);
        if (!modern26)
        {
            const std::vector<std::string> legacyCompatibility = {
                "WINEHUA_DXVK_RELAXED_FEATURES=1",
                "DXVK_WINEHUA_COMMAND_QUERY_RESET=1",
                "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1",
                "DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=auto",
                "DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1",
            };
            for (const std::string& line : legacyCompatibility) UpsertEnvLine(env, line);
        }
        return;
    }
    if (d3dBackend.rfind("dxvk_", 0) != 0) return;

    std::string profile = d3dBackend.substr(strlen("dxvk_"));
    if (profile.empty()) profile = "legacy";
    const bool legacy = profile == "legacy";
    const bool modern26 = profile == "modern_2_6";
    if (!legacy && !modern26) return;
    const std::string runtimeProfile = modern26 ? "modern-2.6" : "legacy";
    const std::string overlayRoot = std::string(WINE_RUNTIME_ROOT) +
        "/dxvk/" + runtimeProfile;
    const std::string overlay64 = overlayRoot + "/x64";
    const std::string overlay86 = overlayRoot + "/x86";
    const std::string guestVulkanRoot = binDir + "/guest_vulkan";
    const std::string guestVulkanLib = guestVulkanRoot + "/lib";
    const std::string guestVulkanIcd = guestVulkanRoot +
        "/share/vulkan/icd.d/venus_icd.x86_64.json";
    const std::string box64LibraryPath = guestVulkanLib + ":" +
        binDir + "/guest_gfx/lib:" + binDir + ":" +
        binDir + "/x86_64-unix:" + std::string(WINE_RUNTIME_ROOT) + "/lib/x86_64";
    const std::string wineDllPath = overlay64 + ":" + overlay86 + ":" +
        binDir + "/x86_64-windows:" + binDir + "/i386-windows:" + binDir;

    const std::vector<std::string> managed = {
        "WINEHUA_D3D_BACKEND=" + d3dBackend,
        "WINEHUA_DXVK_ROOT=" + overlayRoot,
        "WINEHUA_DXVK_PROFILE=" + runtimeProfile,
        "WINEHUA_DXVK_VERSION=" + std::string(modern26 ? "2.6.2" : "1.10.3"),
        "WINEHUA_VULKAN_RUNTIME=1",
        "WINEHUA_VULKAN_LOADER_ARCH=x86_64",
        "WINEHUA_VENUS_ICD_ARCH=x86_64",
#ifdef __aarch64__
        "USE_LIBBOX64=1",
#endif
#ifdef __aarch64__
        "BOX64_LD_LIBRARY_PATH=" + box64LibraryPath,
        "BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:"
            "libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:"
            "libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:"
            "libwayland-client.so:libwayland-client.so.0:libwayland-server.so:"
            "libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:"
            "libdrm.so:libdrm.so.2:libffi.so:libffi.so.8:"
            // GStreamer 链 (winegstreamer): glib + gst core/base + bad/ugly 依赖库
            "libglib-2.0.so:libglib-2.0.so.0:"
            "libgobject-2.0.so:libgobject-2.0.so.0:"
            "libgio-2.0.so:libgio-2.0.so.0:"
            "libgmodule-2.0.so:libgmodule-2.0.so.0:"
            "libgstreamer-1.0.so:libgstreamer-1.0.so.0:"
            "libgstbase-1.0.so:libgstbase-1.0.so.0:"
            "libgstvideo-1.0.so:libgstvideo-1.0.so.0:"
            "libgstaudio-1.0.so:libgstaudio-1.0.so.0:"
            "libgsttag-1.0.so:libgsttag-1.0.so.0:"
            "libgstpbutils-1.0.so:libgstpbutils-1.0.so.0:"
            "libgstallocators-1.0.so:libgstallocators-1.0.so.0:"
            "libgstapp-1.0.so:libgstapp-1.0.so.0:"
            "libgstcontroller-1.0.so:libgstcontroller-1.0.so.0:"
            "libgstfft-1.0.so:libgstfft-1.0.so.0:"
            "libgstnet-1.0.so:libgstnet-1.0.so.0:"
            "libgstriff-1.0.so:libgstriff-1.0.so.0:"
            "libgstrtp-1.0.so:libgstrtp-1.0.so.0:"
            "libgstrtsp-1.0.so:libgstrtsp-1.0.so.0:"
            "libgstsdp-1.0.so:libgstsdp-1.0.so.0:"
            // bad/ugly 插件依赖: videoparsersbad 需 codecparsers, mpegtsdemux 需 mpegts
            "libgstcodecparsers-1.0.so:libgstcodecparsers-1.0.so.0:"
            "libgstmpegts-1.0.so:libgstmpegts-1.0.so.0:"
            "libxml2.so:libxml2.so.2:libz.so:libz.so.1",
#endif
        "VK_DRIVER_FILES=" + guestVulkanIcd,
        "VK_ICD_FILENAMES=" + guestVulkanIcd,
        "VN_DEBUG=vtest",
        /* Host GPU writes to Venus feedback buffers are not automatically
         * visible through WineHua's explicit Guest/Host shadow mapping.
         * Query the real Host objects instead of polling stale Guest words. */
        /* This Guest Mesa/Host virglrenderer runtime uses WineHua's remote
         * shared-ring transport. Per-thread Venus rings can corrupt that
         * transport (the Host decoder observes an invalid command length),
         * so advertise the runtime capability here for every DXVK version.
         * Re-enable multi-ring only after a replacement Venus runtime passes
         * the x86/x64 command-stream qualification gate. */
        "VN_PERF=" + std::string(modern26
            ? "no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring"
            : "no_fence_feedback,no_query_feedback,no_multi_ring"),
        "WINEDLLOVERRIDES=d3d11=n;dxgi=n",
        "VN_WINEHUA_REMOTE_MEMORY_SYNC=1",
        "WINEDLLPATH=" + wineDllPath,
        "WINEDLLDIR0=" + overlay64,
        "WINEDLLDIR1=" + overlay86,
        /* Preserve the contiguous Wine PE runtime search path after the
         * selected DXVK overlays. */
        "WINEDLLDIR2=" + binDir + "/x86_64-windows",
        "WINEDLLDIR3=" + binDir + "/i386-windows",
        "WINEDLLDIR4=" + binDir,
    };
    for (const std::string& line : managed) UpsertEnvLine(env, line);
    if (!legacy) return;

    const std::vector<std::string> legacyCompatibility = {
        "WINEHUA_DXVK_RELAXED_FEATURES=1",
        "DXVK_WINEHUA_COMMAND_QUERY_RESET=1",
        "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1",
        /* Prefer the native RGBA8 SNORM render-target path. On devices such
         * as Maleoon where sampling is supported but color attachment usage
         * is not, DXVK may substitute its qualified RGBA16F backing image. */
        "DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=auto",
        "DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1",
    };
    for (const std::string& line : legacyCompatibility) UpsertEnvLine(env, line);
}

static bool ShouldSerializeEntryParamEnv(const std::string& envLine) {
    return envLine.rfind("WINE_OHOS_AUDIO_ENABLE=", 0) != 0 &&
           envLine.rfind("WINE_OHOS_AUDIO_BOOTSTRAP_FD=", 0) != 0 &&
           envLine.rfind("WINE_OHOS_AUDIO_PROTOCOL_VERSION=", 0) != 0 &&
           envLine.rfind("WINESERVERSOCKET=", 0) != 0;
}

static std::string EnvKey(const std::string& envLine) {
    size_t sep = envLine.find('=');
    return sep == std::string::npos ? envLine : envLine.substr(0, sep);
}

static bool IsBrokerSessionAuthoritativeKey(const std::string& key) {
    // Explorer may start before VirGL is ready. Replace its early Box64 path
    // with the finalized path, where guest graphics libraries are a fallback.
    return key == "BOX64_LD_LIBRARY_PATH";
}

size_t AppendMissingEntryParamsEnvOverrides(std::string& entryParams,
                                            const std::vector<std::string>& env) {
    std::unordered_set<std::string> existingKeys;
    size_t pos = 0;

    while ((pos = entryParams.find("|__env=", pos)) != std::string::npos) {
        pos += strlen("|__env=");
        size_t end = entryParams.find('|', pos);
        std::string key = EnvKey(entryParams.substr(pos, end == std::string::npos
                                                          ? std::string::npos
                                                          : end - pos));
        if (!key.empty()) existingKeys.insert(std::move(key));
        if (end == std::string::npos) break;
        pos = end;
    }

    size_t appended = 0;
    for (const std::string& envLine : env) {
        if (!ShouldSerializeEntryParamEnv(envLine) ||
            envLine.find('|') != std::string::npos ||
            envLine.find('\n') != std::string::npos)
            continue;
        // 过滤 per-process fd 变量: 子进程会从 fdList 拿到自己的值
        if (envLine.rfind("WINESERVERSOCKET=", 0) == 0 ||
            envLine.rfind("WINE_OHOS_AUDIO_ENABLE=", 0) == 0 ||
            envLine.rfind("WINE_OHOS_AUDIO_BOOTSTRAP_FD=", 0) == 0 ||
            envLine.rfind("WINE_OHOS_AUDIO_PROTOCOL_VERSION=", 0) == 0)
            continue;
        const std::string key = EnvKey(envLine);
        if (key.empty() ||
            (existingKeys.count(key) && !IsBrokerSessionAuthoritativeKey(key)))
            continue;
        entryParams += "|__env=";
        entryParams += envLine;
        existingKeys.insert(key);
        ++appended;
    }
    return appended;
}

std::string SerializeEnvToEntryParams(const std::vector<std::string>& env) {
    std::string result;
    for (const std::string& e : env) {
        if (e.find('|') != std::string::npos || e.find('\n') != std::string::npos)
            continue;
        if (e.rfind("WINESERVERSOCKET=", 0) == 0 ||
            e.rfind("WINE_OHOS_AUDIO_ENABLE=", 0) == 0 ||
            e.rfind("WINE_OHOS_AUDIO_BOOTSTRAP_FD=", 0) == 0 ||
            e.rfind("WINE_OHOS_AUDIO_PROTOCOL_VERSION=", 0) == 0)
            continue;
        result += "|__env=";
        result += e;
    }
    return result;
}

void LogGraphicsBackendStateForLaunch(const char* tag) {
    winehua::GraphicsBackendState state = winehua::GraphicsBroker::GetInstance().GetState();
    OH_LOG_INFO(LOG_APP,
                "[%{public}s] graphics requested=%{public}s active=%{public}s runtimeReady=%{public}s "
                "guestReceiver=%{public}s(%{public}s) virglSocketReady=%{public}s virglLibraryPresent=%{public}s",
                tag,
                winehua::GraphicsBroker::BackendName(state.requested),
                winehua::GraphicsBroker::BackendName(state.active),
                state.runtimeReady ? "true" : "false",
                state.guestReceiverPresent ? "true" : "false",
                state.guestReceiverMode.empty() ? "stock-egl" : state.guestReceiverMode.c_str(),
                state.virglSocketReady ? "true" : "false",
                state.virglLibraryPresent ? "true" : "false");
    if (!state.lastError.empty())
        OH_LOG_WARN(LOG_APP, "[%{public}s] graphics note: %{public}s", tag, state.lastError.c_str());
}
