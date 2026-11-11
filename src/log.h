#pragma once

#include <string>

// Tiny file logger. Writes to ds3coopmod.log next to dinput8.dll.
namespace logx {
void Init(const std::wstring& path);
void Write(const char* fmt, ...);
}  // namespace logx

#define LOG(...) logx::Write(__VA_ARGS__)
