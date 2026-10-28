#pragma once

// 硬件指纹采集（对应 C# MachineCodeHelper.cs）
//
// 机器码 = MD5( CPU_ID + 主板序列号 + 物理网卡MAC列表 ) 的小写十六进制。
// 算法与 .NET 版逐字节等价，否则老授权会全部失效。
//
// 注意：C# 中返回 null 表示"未取到"，本实现统一用空串表示（调用方的判定逻辑一致）。

#include <string>
#include <vector>
#include <map>

namespace ps
{
namespace MachineCode
{
    // 生成机器码（32 位小写十六进制）
    std::string GenerateMachineCode();

    // 前 16 位 + "..." + 后 16 位
    std::string GetMachineCodeShort();

    // 物理网卡 MAC 列表（已排序、去重、去分隔符、大写）
    std::vector<std::string> GetPhysicalMacList();
    // 所有物理网卡 MAC 用 "-" 连接；无网卡时返回空串（对应 C# 的 null）
    std::string GetPhysicalMac();

    std::string GetCpuId();
    std::string GetCpuName();
    std::string GetMotherboardSerial();
    std::string GetMotherboardModel();
    // 主板 UUID（SMBIOS）：换硬盘/网卡都不变，是加权指纹里权重最高的稳定因子
    std::string GetMotherboardUuid();
    // BIOS 序列号：与主板同层，换盘换网卡不变
    std::string GetBiosSerial();
    std::string GetSystemDiskSerial();
    std::string GetSystemDiskModel();
    std::string GetOsVersion();
    // 原生版本无 CLR，此处返回本机工具链标识（协议字段名保持 dotnet_version 不变）
    std::string GetDotNetVersion();

    // 上报服务端用的完整硬件信息
    // machineCode 为空时自动生成（对应 C# 的 machineCode ?? GenerateMachineCode()）
    std::map<std::string, std::string> GetHardwareInfo(const std::string& machineCode);
}
}
