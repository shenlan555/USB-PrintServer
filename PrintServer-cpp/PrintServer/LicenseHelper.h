#pragma once

// 授权核心逻辑（对应 C# LicenseHelper.cs）
//
// 职责：授权状态管理、联网验证、在线/离线激活、本地缓存（DPAPI 加密的 license.dat）、
//       定时复验、硬件信息上报。
//
// 本地文件（与 C# 一致，均位于程序目录）：
//   license.dat      DPAPI 加密的本地授权缓存
//   .pending_report  离线激活后待补报标记

#include <string>
#include <functional>
#include <vector>

namespace ps
{
namespace License
{

enum class Status
{
    Unknown,
    Licensed,
    Unlicensed,
    Expired,
    Disabled,
    NetworkError
};

class ActivationResult
{
public:
    bool success = false;
    std::string message;

    ActivationResult() = default;
    ActivationResult(bool ok, const std::string& msg) : success(ok), message(msg) {}
};

// 授权状态变更回调（对应 C# 的 StatusChanged 事件）
using StatusChangedHandler = std::function<void(Status)>;

// ---------------- 生命周期 ----------------

// 初始化：配置 HTTP 客户端、读取服务器地址、加载本地缓存
void Initialize();

// 启动授权验证（程序入口调用）。返回 true 表示授权有效
bool StartValidation();

void StopValidation();

// 手动检查授权（用户点击按钮）
bool ManualCheck();

void SetStatusChangedHandler(StatusChangedHandler handler);

// ---------------- 激活 ----------------

ActivationResult ActivateOnline(const std::string& licenseKey);

// filePath 为 UTF-16 路径
ActivationResult ActivateOffline(const std::wstring& filePath);

// 上报本机硬件信息
bool ReportMachine();

bool HasPendingReport();

// ---------------- 状态查询 ----------------

Status CurrentStatus();
std::string CurrentLicenseKey();
std::string LicenseExpiry();

bool HasLocalLicenseFile();

// 本地授权文件被删除时强制回到未授权状态
void ForceLicenseReactivation();

// 本机机器码（带缓存，与 C# 的 _machineCode 一致）
std::string MachineCode();

// ---------------- 文案 ----------------

std::string GetStatusText();
std::string GetLicenseExpiryText();
bool IsExpiryNever();
int GetExpiryRemainingDays();

// ---------------- 内部 ----------------
namespace Internal
{
    void OnStatusChanged(Status status);
    std::string GetDefaultSecretKey();
    int GetLicenseValidationIntervalHours();
    std::string BuildJson(const std::vector<std::pair<std::string, std::string>>& pairs);
    std::string HttpPost(const std::string& url, const std::string& json, int timeoutSeconds);
    bool ValidateWithServerSync();
    bool ValidateLocalLicense();
    bool IsServerReachable();
    std::wstring LicenseFilePath();
    std::wstring PendingReportPath();
}

// ---------------- 内部实现（需在使用点之前可见） ----------------
void SaveLocalLicense(const std::string& licenseKey, const std::string& method);
void LoadLocalLicense();
void StartValidationTimer();
void OnValidationTimer();

}
}
