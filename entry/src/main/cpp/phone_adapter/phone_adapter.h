// phone_adapter.h — 手机适配层扩展 API（非标准 NCP API）
// 这些函数/宏不在系统 <AbilityKit/native_child_process.h> 中，
// 仅在手机设备上使用。系统 NCP 类型从系统头文件获取。
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PHONE_ADAPTER_DUMMY_PROXY ((void*)0x4E435031)  // 标识非真 IPC proxy（手机 fork 路径专用）

// ====== 手机设备标志 ======
// 由 EntryAbility 在 onWindowStageCreate 中根据 deviceType==='phone' 设置。
// 必须在首次 NCP 调用前就绪（首次调用在 Index.doInit 异步回调中）。
void PhoneAdapter_SetPhoneMode(bool phone);

// ====== 查询接口（graphics_broker.cpp 使用） ======
bool PhoneAdapter_IsDummyProxy(const void* p);
int  PhoneAdapter_GetConfigSocket(void);
void PhoneAdapter_CloseConfigSocket(void);

#ifdef __cplusplus
}
#endif
