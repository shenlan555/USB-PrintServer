#pragma once

// 进程级辅助（对应 C# Program.cs 的 GetDisplayVersion 与路径获取）

#include <string>
#include <vector>

namespace ps
{
namespace ProgramHost
{

// 本进程可执行文件完整路径
std::wstring GetExecutablePath();

// 所在目录（带结尾反斜杠），对应 C# 的 AppDomain.CurrentDomain.BaseDirectory
std::wstring GetExecutableDirectory();

// 对应 Program.GetDisplayVersion()：读取自身版本资源，Revision<=0 时只显示 3 段
std::string GetDisplayVersion();

// 当前进程令牌是否已提权（对应服务端 ProgramHost::IsAdministrator）
bool IsAdministrator();

// 以管理员身份重新启动本进程（沿用同一命令行参数）。
// 启动成功返回 true（调用方应立即退出当前实例）；
// 用户在 UAC 对话框点「否」时返回 false，并已弹出提示。
bool RelaunchElevated(const std::vector<std::wstring>& args);

} // namespace ProgramHost
} // namespace ps
