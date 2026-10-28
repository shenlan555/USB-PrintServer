#pragma once

// 反调试与惩罚机制（对应 C# AntiDebug.cs）
//
// 与 LicenseHelper 是双向依赖：
//   - 本模块读取 LicenseHelper 的状态用于篡改检测
//   - LicenseHelper 在本模块埋检测点（Checkpoint）并在服务端判定禁用/过期时触发惩罚

#include <string>

namespace ps
{
namespace AntiDebug
{
    // 初始化：检查注册表黑名单（命中则直接退出）并启动守护线程
    void Initialize();

    // 完整检测（带 30 秒频率限制与 3 次阈值去抖）
    bool CheckAll();

    // 检测点：命中则记录并执行惩罚
    void Checkpoint(const std::string& location);

    void StartGuardian();
    void StopGuardian();

    // 注册表黑名单：HKCU\Software\PrintServer\Blacklisted
    bool IsBlacklisted();

    // 强制惩罚（供授权模块在服务端返回禁用/过期时调用）
    void ForcePenalty();

    // 授权系统是否被篡改
    bool IsLicenseSystemTampered();
}
}
