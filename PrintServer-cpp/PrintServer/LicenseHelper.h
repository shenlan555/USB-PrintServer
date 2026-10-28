#pragma once

// 授权系统 —— 开源免费版存根
//
// 原商业授权逻辑（在线/离线激活、联网验证、license.dat 缓存、机器码采集、
// 定时复验）已全部移除，本文件只保留主界面与服务宿主所需的最小接口，
// 状态恒为 Licensed（免费版）。

#include <string>
#include <functional>

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

using StatusChangedHandler = std::function<void(Status)>;

// ---------------- 生命周期 ----------------

void Initialize();

// 恒返回 true（免费版无需授权验证）
bool StartValidation();

void StopValidation();

// 恒返回 true（免费版无需授权验证）
bool ManualCheck();

void SetStatusChangedHandler(StatusChangedHandler handler);

// ---------------- 状态查询 ----------------

Status CurrentStatus();
std::string CurrentLicenseKey();
std::string LicenseExpiry();

bool HasLocalLicenseFile();

void ForceLicenseReactivation();

std::string GetStatusText();
std::string GetLicenseExpiryText();
bool IsExpiryNever();
int GetExpiryRemainingDays();

}
}
