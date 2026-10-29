#include "pch.h"
#include "LicenseHelper.h"

#include <mutex>

// 授权接口的默认实现：不联网、不采集硬件信息、不读写授权文件，
// 状态恒为 Licensed、有效期视为永久。

namespace ps
{
namespace License
{

namespace
{
std::mutex g_mutex;
StatusChangedHandler g_statusChangedHandler;
}

void Initialize()
{
    // 无授权上下文需要初始化
}

bool StartValidation()
{
    return true;
}

void StopValidation()
{
}

bool ManualCheck()
{
    return true;
}

void SetStatusChangedHandler(StatusChangedHandler handler)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    g_statusChangedHandler = std::move(handler);
}

Status CurrentStatus()
{
    return Status::Licensed;
}

std::string CurrentLicenseKey()
{
    return std::string();
}

std::string LicenseExpiry()
{
    return std::string(); // 空 = 永不过期
}

bool HasLocalLicenseFile()
{
    return true; // 不存在本地授权文件
}

void ForceLicenseReactivation()
{
    // 无授权状态可重置
}

std::string GetStatusText()
{
    return "已授权";
}

std::string GetLicenseExpiryText()
{
    return "有效期: 永不过期";
}

bool IsExpiryNever()
{
    return true;
}

int GetExpiryRemainingDays()
{
    return 9999;
}

}
}
