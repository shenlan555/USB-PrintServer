#pragma once

// 进程级编排（对应 C# Program.cs）
//
// 职责：版本号、管理员权限检查与提权、服务安装/卸载、托盘助手自启与拉起。

#include <string>
#include <vector>

namespace ps
{
namespace ProgramHost
{

// 本进程可执行文件完整路径
std::wstring GetExecutablePath();

// 对应 Program.GetDisplayVersion()：读取自身版本资源，Revision<=0 时只显示 3 段
std::string GetDisplayVersion();
std::string GetDisplayVersionWithPrefix();

// 对应 Program.IsAdministrator()
bool IsAdministrator();

// 对应 Program.RestartAsAdmin(args)：以 runas 提权重启自身，随后调用方须退出
void RestartAsAdmin(const std::vector<std::wstring>& args);

// 对应 Program.SetTrayAutoStart(enable)：HKCU\...\Run 下的 PrintServerTray
void SetTrayAutoStart(bool enable);

// 对应 Program.LaunchTrayHelper()：启动 "\"<exe>\" /tray"
void LaunchTrayHelper();

// 对应 Program.InstallService() / UninstallService()
void InstallService();
void UninstallService();

// ---- 服务状态（走 SCM API，不依赖 sc 输出的本地化文本，也不需要管理员权限）----

enum class ServiceState
{
    NotInstalled,   // 服务不存在（1060）
    Stopped,
    StartPending,
    StopPending,
    Running,
    Paused,
    Unknown         // 查询失败或服务已被标记为删除（1072）
};

ServiceState QueryServiceState();

// 等待服务真正到达 STOPPED：sc stop 只是下达停止请求，返回时服务往往还在 STOP_PENDING，
// 此时删除服务会被 SCM 挂起（服务被标记为“待删除”而不真正移除）
bool WaitForServiceStopped(int timeoutMs);

// 等待服务条目从 SCM 数据库中消失（删除是否真正生效）
bool WaitForServiceRemoved(int timeoutMs);

// 请求托盘助手（窗口标题 PrintServerTray）退出并等待其关闭。
// 托盘助手长期持有 PrintServer 的服务句柄，而 SCM 只有在“服务已停止 + 所有服务句柄已关闭”
// 之后才会把已标记删除的服务真正移除；不先关掉它，sc delete 只能把服务标记为待删除，
// 再次 delete 就会返回 1072（指定的服务已标记为删除）。
// 返回 true 表示检测到了托盘助手并且它已经退出。
bool RequestTrayHelperExit(int timeoutMs);

} // namespace ProgramHost
} // namespace ps
