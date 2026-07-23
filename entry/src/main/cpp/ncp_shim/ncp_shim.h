// ncp_shim.h — WineHua fork-shim 扩展（非标准 NCP API）
// 这些函数/宏不在系统 <AbilityKit/native_child_process.h> 中，
// 仅在手机 fork-shim 路径下使用。系统 NCP 类型从系统头文件获取。
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OH_NCP_SHIM_DUMMY_PROXY ((void*)0x4E435031)  // 'NCP1' 魔数，标识 fake proxy

// ====== 手机设备标志 ======
// 由 EntryAbility 在 onWindowStageCreate 中根据 deviceType==='phone' 设置。
// 必须在首次 NCP 调用前就绪（首次调用在 Index.doInit 异步回调中）。
void OH_NCPShim_SetPhoneMode(bool phone);

// ====== 查询接口（graphics_broker.cpp 使用） ======
bool OH_NCPShim_IsDummyProxy(const void* p);
int  OH_NCPShim_GetConfigSocket(void);
void OH_NCPShim_CloseConfigSocket(void);

#ifdef __cplusplus
}
#endif
