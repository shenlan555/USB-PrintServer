#include "pch.h"
#include "LicenseHelper.h"

#include <mutex>

// 授权系统 —— 开源免费版存根实现
// 原商业授权逻辑已整体移除：不联网、不采集硬件指纹、不生成任何本地授权文件。

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
    // 免费版：无需初始化授权上下文
}

bool StartValidation()
{
    return true; // 免费版：启动即视为已授权
}

void StopValidation()
{
}

bool ManualCheck()
{
    return true; // 免费版：无需授权验证
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
    return std::string(); // 免费版无授权码
}

std::string LicenseExpiry()
{
    return std::string(); // 空 = 永不过期
}

bool HasLocalLicenseFile()
{
    return true; // 免费版：无 license.dat 概念
}

void ForceLicenseReactivation()
{
    // 免费版：无授权状态可重置
}

std::string GetStatusText()
{
    return "免费版（无需授权）";
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
