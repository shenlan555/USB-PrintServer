#pragma once

// 局域网扫描器（对应 C# LanScanner.cs）
//
// 扫描当前网段中运行 PrintServer 的主机。
// C# 用 NetworkInterface 枚举网卡，这里改用 GetAdaptersAddresses（iphlpapi），
// 反向解析主机名用 getnameinfo；算法、分批、超时与提示文本与 C# 完全一致。

#include <string>
#include <vector>
#include <functional>

namespace ps
{

struct ScanResult
{
    std::string ipAddress;
    std::string hostname;
    bool isReachable = false;
    int apiPort = 0;
};

// 对应 IProgress<string>：消息为 UTF-8
using ScanProgressFn = std::function<void(const std::string&)>;

// 对应 LanScanner.ScanAsync(apiPort, progress)
std::vector<ScanResult> ScanAsync(int apiPort, const ScanProgressFn& progress);

// 对应 LanScanner.GetLocalSubnetPrefixes()，返回如 "192.168.1." 的前缀
std::vector<std::string> GetLocalSubnetPrefixes();

// 确保当前进程已完成 WSAStartup（可重复调用）
void EnsureWinsockInitialized();

} // namespace ps
