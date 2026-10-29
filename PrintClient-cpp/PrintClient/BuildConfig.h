#pragma once

#include <string>

// 编译期常量：严格取自 C# 的 AssemblyInfo.cs
// （版本号本身从 exe 的 VERSIONINFO 读取，见 Program.h 的 GetDisplayVersion）

namespace ps
{
namespace BuildConfig
{

// [assembly: AssemblyTitle("网络打印安装工具")]
inline const std::string Title = "\xE7\xBD\x91\xE7\xBB\x9C\xE6\x89\x93\xE5\x8D\xB0\xE5\xAE\x89\xE8\xA3\x85\xE5\xB7\xA5\xE5\x85\xB7";

// [assembly: AssemblyProduct("网络打印安装工具")]
inline const char* ProductName = "\xE7\xBD\x91\xE7\xBB\x9C\xE6\x89\x93\xE5\x8D\xB0\xE5\xAE\x89\xE8\xA3\x85\xE5\xB7\xA5\xE5\x85\xB7";

// [assembly: AssemblyCompany("浪里个浪")]
inline const char* CompanyName = "\xE6\xB5\xAA\xE9\x87\x8C\xE4\xB8\xAA\xE6\xB5\xAA";

// [assembly: AssemblyDescription("网络打印安装工具 - 自动发现、一键安装、即装即用")]
inline const char* Description =
    "\xE7\xBD\x91\xE7\xBB\x9C\xE6\x89\x93\xE5\x8D\xB0\xE5\xAE\x89\xE8\xA3\x85\xE5\xB7\xA5\xE5\x85\xB7 - "
    "\xE8\x87\xAA\xE5\x8A\xA8\xE5\x8F\x91\xE7\x8E\xB0\xE3\x80\x81\xE4\xB8\x80\xE9\x94\xAE\xE5\xAE\x89\xE8\xA3\x85\xE3\x80\x81"
    "\xE5\x8D\xB3\xE8\xA3\x85\xE5\x8D\xB3\xE7\x94\xA8";

// [assembly: AssemblyVersion("1.4.3")] —— 资源损坏时的兜底显示值
inline const char* FallbackVersion = "1.4.3";

// 默认 API 端口（对应 C# NumericUpDown.Value = 9180）
inline const int DefaultApiPort = 9180;

// /api/driver 下载鉴权共享密钥（HMAC-SHA256）。
// 必须与 PrintServer/BuildConfig.h 中的 ApiAuthSecret 完全一致。
inline constexpr const char* ApiAuthSecret =
    "vsM2qFQq2wD1oTWd+bVfF27H0GHQ7Qk4klbTlgRXmDr0TswxiKdQg8BdOGkGgGzL";

} // namespace BuildConfig
} // namespace ps
