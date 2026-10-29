#pragma once

// 授权接口
//
// 主界面与服务宿主只依赖这里的接口，具体实现见 LicenseHelper.cpp。
// 本版本不带联网校验与授权文件：状态恒为 Licensed（有效期视为永久）。

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

// 恒返回 true（本版本无授权校验）
bool StartValidation();

void StopValidation();

// 恒返回 true（本版本无授权校验）
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
