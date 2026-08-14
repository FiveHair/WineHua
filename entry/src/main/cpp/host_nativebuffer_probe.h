#pragma once

#include <cstdint>
#include <string>

bool StartHostNativeBufferProbe(uint64_t surfaceId, const std::string& runId);
void StopHostNativeBufferProbe();
