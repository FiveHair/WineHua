#ifndef WINE_ENV_H
#define WINE_ENV_H

/**
 * wine_env.h — Wine 环境变量设置
 */

#include <cstdlib>
#include <string>
#include <vector>

#include "wine_constants.h"

// -- Wine 环境变量构建 --
// wineLang: Wine locale 语言 ("zh_CN"/"en_US"), 决定基线 LANG=<wineLang>.UTF-8;
// 桌面会话经 launchClient 由用户设置传入, 程序直启路径由 ArkTS 经 environment
// 覆盖 (UpsertEnvLine 后于基线生效), 其余调用点保持默认中文
std::vector<std::string> BuildWineEnv(const std::string& sockDir,
                                      const std::string& sockName,
                                      const std::string& libPath,
                                      const std::string& binDir,
                                      int audioBootstrapFd,
                                      const std::string& homeDir,
                                      const std::string& prefixDir = WINE_PREFIX,
                                      const std::string& wineLang = "zh_CN");

// Add the managed product D3D backend overlay to a process environment. The
// caller selects the product backend once per Wine session; the default is
// dxvk_legacy, while wined3d remains an explicit compatibility fallback.
void AppendD3dBackendEnv(std::vector<std::string>& env,
                         const std::string& d3dBackend,
                         const std::string& binDir);

// -- 环境变量辅助 --
void UpsertEnvLine(std::vector<std::string>& env, const std::string& line);

// -- Audio bootstrap --
int CreateAudioBootstrapFd(const std::string& runtimeDir);

// -- entryParams 序列化 --
size_t AppendMissingEntryParamsEnvOverrides(std::string& entryParams,
                                            const std::vector<std::string>& env);
std::string SerializeEnvToEntryParams(const std::vector<std::string>& env);

// -- Graphics 辅助 --
void LogGraphicsBackendStateForLaunch(const char* tag);

#endif // WINE_ENV_H
