#pragma once

#include <cstdint>
#include <string>

bool StartHostNativeWindowProbe(uint64_t surfaceId, const std::string& runId);
void StopHostNativeWindowProbe();
