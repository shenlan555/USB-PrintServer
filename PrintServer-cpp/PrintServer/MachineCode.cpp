#include "pch.h"
#include "MachineCode.h"
#include "Wmi.h"
#include "Crypto.h"
#include "BuildConfig.h"

#include <iphlpapi.h>
#include <algorithm>
#include <string.h>

#pragma comment(lib, "iphlpapi.lib")

#ifndef IF_TYPE_ETHERNET_CSMACD
#define IF_TYPE_ETHERNET_CSMACD 6
#endif
#ifndef IF_TYPE_ETHERNET_3MEGABIT
#define IF_TYPE_ETHERNET_3MEGABIT 26
#endif
#ifndef IF_TYPE_SOFTWARE_LOOPBACK
#define IF_TYPE_SOFTWARE_LOOPBACK 24
#endif

namespace ps
{
namespace MachineCode
{

// ===========================================================================
// 内部工具
// ===========================================================================

namespace
{

std::string Trim(const std::string& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

std::string ToUpperAscii(const std::string& s)
{
    std::string out = s;
    for (char& c : out)
    {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
    }
    return out;
}

// 去掉 '-' '/' '.'
std::string StripPunct(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        if (c != '-' && c != '/' && c != '.')
            out.push_back(c);
    }
    return out;
}

// 去掉指定字符集（用于 MAC 去除分隔符）
std::string RemoveChars(const std::string& s, const char* chars)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        if (strchr(chars, c) == nullptr)
            out.push_back(c);
    }
    return out;
}

bool VecContains(const std::vector<std::string>& v, const std::string& item)
{
    for (const auto& s : v)
    {
        if (s == item)
            return true;
    }
    return false;
}

std::string Join(const std::vector<std::string>& v, const char* sep)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i > 0)
            out += sep;
        out += v[i];
    }
    return out;
}

// 对应 C# NormalizeHardwareId()
std::string NormalizeHardwareId(const std::string& rawUtf8, const char* fallback)
{
    if (rawUtf8.empty())
        return fallback;

    std::string normalized = ToUpperAscii(Trim(rawUtf8));

    static const char* kInvalidPatterns[] = {
        "00000000", "0000000000000000", "FFFFFFFF",
        "TO BE FILLED BY O.E.M.", "TO BE FILLED BY OEM",
        "NONE", "NULL", "UNKNOWN", "NOT AVAILABLE",
        "NOT APPLICABLE", "CHASSIS SERIAL NUMBER",
        "SYSTEM SERIAL NUMBER", "BASE BOARD SERIAL NUMBER",
        "0123456789ABCDEF", "1234567890",
        ".", "-", "/", " "
    };

    for (const char* pattern : kInvalidPatterns)
    {
        if (normalized == pattern || StripPunct(normalized) == StripPunct(pattern))
            return fallback;
    }

    // 只保留字母数字（对应 Regex.Replace(normalized, "[^A-Z0-9]", "")）
    std::string cleaned;
    cleaned.reserve(normalized.size());
    for (char c : normalized)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            cleaned.push_back(c);
    }

    if (cleaned.size() < 3)
        return fallback;

    return cleaned;
}

// ---------------- 网卡关键字过滤 ----------------

bool IsVirtualAdapter(const std::string& description)
{
    static const char* kKeywords[] = {
        "virtual", "vmware", "virtualbox", "hyper-v", "hyperv",
        "vpn", "tap", "tun", "ndis", "pseudo", "loopback",
        "bluetooth", "wsl", "docker", "hamachi", "openvpn"
    };

    for (const char* kw : kKeywords)
    {
        if (ContainsIgnoreCaseAscii(description, kw))
            return true;
    }
    return false;
}

bool IsSystemGeneratedAdapter(const std::string& description)
{
    static const char* kKeywords[] = {
        "wi-fi direct", "wifi direct", "wireless direct",
        "teredo", "isatap", "6to4",
        "wan miniport", "wan mini-port",
        "kernel debug", "kdnic",
        "microsoft hosted network", "hosted network",
        "microsoft wi-fi", "microsoft wifi",
        "ras sync", "pppoe", "pptp", "l2tp", "sstp",
        "microsoft network adapter multiplexor",
        "microsoft hyper-v network", "hyper-v network",
        "bridge", "bridge port"
    };

    for (const char* kw : kKeywords)
    {
        if (ContainsIgnoreCaseAscii(description, kw))
            return true;
    }
    return false;
}

bool IsUsbNetworkAdapter(const std::string& description)
{
    static const char* kKeywords[] = {
        "usb", "usb-c", "usb c",
        "thunderbolt",
        "dock", "docking",
        "realtek usb", "asix", "ax88179", "ax88772",
        "rtl815",
        "802.11ac wireless lan"
    };

    for (const char* kw : kKeywords)
    {
        if (ContainsIgnoreCaseAscii(description, kw))
            return true;
    }
    return false;
}

// ---------------- WMI 单值读取 ----------------

// 对应 C#：遍历查询结果，返回第一个满足条件的字符串
std::string QueryFirstString(const wchar_t* wql, const wchar_t* prop)
{
    std::string result;
    WmiQuery q;
    if (!q.IsValid())
        return result;

    q.ForEach(wql, [&](IWbemClassObject* obj) -> bool {
        std::wstring value;
        if (WmiQuery::GetString(obj, prop, value))
        {
            std::string text = Trim(WideToUtf8(value));
            if (!text.empty())
            {
                result = text;
                return false; // 找到即停止
            }
        }
        return true;
    });

    return result;
}

// ---------------- 虚拟机检测（对应 C# IsRunningInVM）----------------

bool IsRunningInVM()
{
    bool inVm = false;

    // 方式1：主板型号
    {
        WmiQuery q;
        if (q.IsValid())
        {
            q.ForEach(L"SELECT Product, Manufacturer FROM Win32_BaseBoard",
                      [&](IWbemClassObject* obj) -> bool {
                std::wstring wProduct, wManufacturer;
                std::string product = WmiQuery::GetString(obj, L"Product", wProduct)
                    ? ToLowerAscii(WideToUtf8(wProduct)) : std::string();
                std::string manufacturer = WmiQuery::GetString(obj, L"Manufacturer", wManufacturer)
                    ? ToLowerAscii(WideToUtf8(wManufacturer)) : std::string();

                if (product.find("vmware") != std::string::npos ||
                    manufacturer.find("vmware") != std::string::npos) { inVm = true; return false; }

                if (product.find("virtualbox") != std::string::npos ||
                    manufacturer.find("virtualbox") != std::string::npos ||
                    product.find("vbox") != std::string::npos) { inVm = true; return false; }

                if (product.find("qemu") != std::string::npos ||
                    product.find("kvm") != std::string::npos ||
                    product.find("standard pc (i440fx") != std::string::npos ||
                    product.find("standard pc (q35") != std::string::npos) { inVm = true; return false; }

                if (product.find("virtual machine") != std::string::npos &&
                    manufacturer.find("microsoft") != std::string::npos) { inVm = true; return false; }

                return true;
            });
        }
    }

    // 方式2：系统制造商
    if (!inVm)
    {
        WmiQuery q;
        if (q.IsValid())
        {
            q.ForEach(L"SELECT Manufacturer, Model FROM Win32_ComputerSystem",
                      [&](IWbemClassObject* obj) -> bool {
                std::wstring wMfr, wModel;
                std::string mfr = WmiQuery::GetString(obj, L"Manufacturer", wMfr)
                    ? ToLowerAscii(WideToUtf8(wMfr)) : std::string();
                std::string model = WmiQuery::GetString(obj, L"Model", wModel)
                    ? ToLowerAscii(WideToUtf8(wModel)) : std::string();

                if (mfr.find("vmware") != std::string::npos ||
                    mfr.find("virtualbox") != std::string::npos ||
                    mfr.find("qemu") != std::string::npos ||
                    mfr.find("kvm") != std::string::npos ||
                    mfr.find("xen") != std::string::npos ||
                    mfr.find("microsoft corporation") != std::string::npos ||
                    model.find("virtual") != std::string::npos ||
                    model.find("vmware") != std::string::npos ||
                    model.find("kvm") != std::string::npos ||
                    model.find("qemu") != std::string::npos) { inVm = true; return false; }

                return true;
            });
        }
    }

    // 方式3：BIOS 信息
    if (!inVm)
    {
        WmiQuery q;
        if (q.IsValid())
        {
            q.ForEach(L"SELECT Manufacturer, SMBIOSBIOSVersion FROM Win32_BIOS",
                      [&](IWbemClassObject* obj) -> bool {
                std::wstring wMfr, wVersion;
                std::string mfr = WmiQuery::GetString(obj, L"Manufacturer", wMfr)
                    ? ToLowerAscii(WideToUtf8(wMfr)) : std::string();
                std::string version = WmiQuery::GetString(obj, L"SMBIOSBIOSVersion", wVersion)
                    ? ToLowerAscii(WideToUtf8(wVersion)) : std::string();

                if (mfr.find("vmware") != std::string::npos ||
                    mfr.find("virtualbox") != std::string::npos ||
                    mfr.find("qemu") != std::string::npos ||
                    mfr.find("xen") != std::string::npos ||
                    version.find("vmware") != std::string::npos ||
                    version.find("vbox") != std::string::npos) { inVm = true; return false; }

                return true;
            });
        }
    }

    return inVm;
}

// ---------------- 网卡 MAC 采集 ----------------

// 从 WMI 收集所有非系统网卡 MAC（VM 回退用，不过滤虚拟网卡）
std::vector<std::string> GetAllNonSystemMacListFromWmi()
{
    std::vector<std::string> macList;

    WmiQuery q;
    if (!q.IsValid())
        return macList;

    q.ForEach(L"SELECT MACAddress, Name, Description FROM Win32_NetworkAdapter",
              [&](IWbemClassObject* obj) -> bool {
        std::wstring wMac;
        if (!WmiQuery::GetString(obj, L"MACAddress", wMac))
            return true;

        std::string mac = Trim(WideToUtf8(wMac));
        if (mac.empty())
            return true;

        std::wstring wName, wDesc;
        std::string name = WmiQuery::GetString(obj, L"Name", wName) ? WideToUtf8(wName) : std::string();
        std::string desc = WmiQuery::GetString(obj, L"Description", wDesc) ? WideToUtf8(wDesc) : std::string();
        std::string fullDesc = desc + " " + name;

        if (IsSystemGeneratedAdapter(fullDesc))
            return true;

        mac = ToUpperAscii(RemoveChars(mac, "-:"));
        if (!VecContains(macList, mac))
            macList.push_back(mac);

        return true;
    });

    return macList;
}

// 通过 IP Helper 枚举网卡（对应 C# NetworkInterface.GetAllNetworkInterfaces）
struct AdapterInfo
{
    std::string description;
    std::string mac;          // 大写十六进制、无分隔符；无地址时为空
    bool isEthernet = false;
    bool isLoopback = false;
};

bool EnumerateAdapters(std::vector<AdapterInfo>& out)
{
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;

    std::vector<BYTE> buffer(size);
    ULONG ret = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                       (IP_ADAPTER_ADDRESSES*)buffer.data(), &size);
    if (ret == ERROR_BUFFER_OVERFLOW)
    {
        buffer.resize(size);
        ret = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                     (IP_ADAPTER_ADDRESSES*)buffer.data(), &size);
    }

    if (ret != NO_ERROR)
        return false;

    static const char* kHex = "0123456789ABCDEF";

    for (IP_ADAPTER_ADDRESSES* p = (IP_ADAPTER_ADDRESSES*)buffer.data();
         p != nullptr; p = p->Next)
    {
        AdapterInfo info;
        info.description = p->Description ? WideToUtf8(p->Description) : std::string();
        info.isEthernet = (p->IfType == IF_TYPE_ETHERNET_CSMACD ||
                           p->IfType == IF_TYPE_ETHERNET_3MEGABIT);
        info.isLoopback = (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK);

        if (p->PhysicalAddressLength > 0 && p->PhysicalAddress != nullptr)
        {
            std::string mac;
            mac.reserve(p->PhysicalAddressLength * 2);
            for (ULONG i = 0; i < p->PhysicalAddressLength; ++i)
            {
                mac.push_back(kHex[p->PhysicalAddress[i] >> 4]);
                mac.push_back(kHex[p->PhysicalAddress[i] & 0x0F]);
            }
            info.mac = mac;
        }

        out.push_back(info);
    }

    return true;
}

// 从 NetworkInterface 等价 API 收集所有非系统网卡 MAC（VM 回退用）
std::vector<std::string> GetAllNonSystemMacListFromNetInterface()
{
    std::vector<std::string> macList;

    std::vector<AdapterInfo> adapters;
    if (!EnumerateAdapters(adapters))
        return macList;

    for (const auto& a : adapters)
    {
        // 对应 C#: if (ni.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
        if (a.isLoopback)
            continue;

        if (IsSystemGeneratedAdapter(a.description))
            continue;

        if (a.mac.empty())
            continue;

        std::string mac = ToUpperAscii(a.mac);
        if (!VecContains(macList, mac))
            macList.push_back(mac);
    }

    return macList;
}

// WMI 路径（PhysicalAdapter=True）
std::vector<std::string> GetPhysicalMacListFromWmi()
{
    std::vector<std::string> macList;

    WmiQuery q;
    if (!q.IsValid())
        return macList;

    q.ForEach(L"SELECT MACAddress, Name, Description, NetEnabled, PhysicalAdapter FROM Win32_NetworkAdapter",
              [&](IWbemClassObject* obj) -> bool {
        std::wstring wMac;
        if (!WmiQuery::GetString(obj, L"MACAddress", wMac))
            return true;

        std::string mac = Trim(WideToUtf8(wMac));
        if (mac.empty())
            return true;

        std::wstring wName, wDesc;
        std::string name = WmiQuery::GetString(obj, L"Name", wName) ? WideToUtf8(wName) : std::string();
        std::string desc = WmiQuery::GetString(obj, L"Description", wDesc) ? WideToUtf8(wDesc) : std::string();
        std::string fullDesc = desc + " " + name;

        // 属性缺失时保持 false（与 C# 一致：只有属性存在且为真才算物理网卡）
        bool isPhysical = false;
        if (!WmiQuery::GetBool(obj, L"PhysicalAdapter", isPhysical))
            isPhysical = false;

        if (!isPhysical)
            return true;

        if (IsSystemGeneratedAdapter(fullDesc))
            return true;

        if (IsUsbNetworkAdapter(fullDesc))
            return true;

        mac = ToUpperAscii(RemoveChars(mac, "-:"));
        if (!VecContains(macList, mac))
            macList.push_back(mac);

        return true;
    });

    // VM 回退
    if (macList.empty() && IsRunningInVM())
        macList = GetAllNonSystemMacListFromWmi();

    return macList;
}

// NetworkInterface 回退路径
std::vector<std::string> GetPhysicalMacListFromNetInterface()
{
    std::vector<std::string> macList;

    std::vector<AdapterInfo> adapters;
    if (!EnumerateAdapters(adapters))
        return macList;

    for (const auto& a : adapters)
    {
        // 对应 C#: 只取 Ethernet 类型（排除无线、隧道、虚拟等）
        if (!a.isEthernet)
            continue;

        if (IsVirtualAdapter(a.description))
            continue;

        if (IsSystemGeneratedAdapter(a.description))
            continue;

        if (IsUsbNetworkAdapter(a.description))
            continue;

        if (a.mac.empty())
            continue;

        std::string mac = ToUpperAscii(a.mac);
        if (!VecContains(macList, mac))
            macList.push_back(mac);
    }

    if (macList.empty() && IsRunningInVM())
        macList = GetAllNonSystemMacListFromNetInterface();

    return macList;
}

} // namespace

// ===========================================================================
// 对外接口
// ===========================================================================

std::string GenerateMachineCode()
{
    std::string cpuId = NormalizeHardwareId(GetCpuId(), "UNKNOWN_CPU");
    std::string boardSn = NormalizeHardwareId(GetMotherboardSerial(), "UNKNOWN_BOARD");
    std::string mac = GetPhysicalMac();
    if (mac.empty())
        mac = "UNKNOWN_MAC";

    std::string raw = ToUpperAscii(cpuId + boardSn + mac);

    std::vector<unsigned char> hash = Crypto::Md5(raw);
    if (hash.empty())
        return std::string();

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(hash.size() * 2);
    for (unsigned char c : hash)
    {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

std::string GetMachineCodeShort()
{
    std::string code = GenerateMachineCode();
    if (code.size() >= 32)
        return code.substr(0, 16) + "..." + code.substr(code.size() - 16);
    return code;
}

std::vector<std::string> GetPhysicalMacList()
{
    std::vector<std::string> list = GetPhysicalMacListFromWmi();
    if (!list.empty())
        return list;

    list = GetPhysicalMacListFromNetInterface();
    if (!list.empty())
        return list;

    return std::vector<std::string>();
}

std::string GetPhysicalMac()
{
    std::vector<std::string> list = GetPhysicalMacList();
    if (list.empty())
        return std::string(); // 对应 C# 的 null

    // 对应 C# macList.Sort(StringComparer.Ordinal)
    std::sort(list.begin(), list.end());
    return Join(list, "-");
}

std::string GetCpuId()
{
    return QueryFirstString(L"SELECT ProcessorId FROM Win32_Processor", L"ProcessorId");
}

std::string GetCpuName()
{
    std::string name = QueryFirstString(L"SELECT Name FROM Win32_Processor", L"Name");
    return name.empty() ? "未知" : name;
}

std::string GetMotherboardSerial()
{
    std::string result;

    WmiQuery q;
    if (q.IsValid())
    {
        q.ForEach(L"SELECT SerialNumber FROM Win32_BaseBoard", [&](IWbemClassObject* obj) -> bool {
            std::wstring wSn;
            if (WmiQuery::GetString(obj, L"SerialNumber", wSn))
            {
                std::string sn = Trim(WideToUtf8(wSn));
                // 与 C# 一致：此处为大小写敏感的精确比较
                if (!sn.empty() && sn != "To be filled by O.E.M.")
                {
                    result = sn;
                    return false;
                }
            }
            return true;
        });
    }

    return result;
}

std::string GetMotherboardModel()
{
    std::string model = QueryFirstString(L"SELECT Product FROM Win32_BaseBoard", L"Product");
    return model.empty() ? "未知" : model;
}

std::string GetMotherboardUuid()
{
    std::string uuid = QueryFirstString(L"SELECT UUID FROM Win32_ComputerSystemProduct", L"UUID");
    uuid = Trim(uuid);

    // BIOS 模板值 / 全 0 / 全 F 视为无效（组装机常见）
    if (uuid.empty())
        return std::string();

    const std::string compact = StripPunct(ToUpperAscii(uuid));
    if (compact == "03000200040005000006000700080009" ||
        compact == "00000000000000000000000000000000" ||
        compact == "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF")
    {
        return std::string();
    }

    return uuid;
}

std::string GetBiosSerial()
{
    std::string sn = QueryFirstString(L"SELECT SerialNumber FROM Win32_BIOS", L"SerialNumber");
    sn = Trim(sn);

    if (sn.empty())
        return std::string();

    const std::string upper = ToUpperAscii(sn);
    static const char* kInvalid[] = {
        "DEFAULT STRING", "TO BE FILLED BY O.E.M.", "TO BE FILLED BY OEM",
        "SYSTEM SERIAL NUMBER", "NOT APPLICABLE", "NONE", "NULL", "INVALID",
    };

    for (const char* item : kInvalid)
    {
        if (upper == item)
            return std::string();
    }

    return sn;
}

std::string GetSystemDiskSerial()
{
    std::string result;

    WmiQuery q;
    if (q.IsValid())
    {
        q.ForEach(L"SELECT SerialNumber, Index FROM Win32_DiskDrive", [&](IWbemClassObject* obj) -> bool {
            std::wstring wIndex;
            std::string index = WmiQuery::GetString(obj, L"Index", wIndex)
                ? Trim(WideToUtf8(wIndex)) : std::string();

            if (index == "0")
            {
                std::wstring wSn;
                if (WmiQuery::GetString(obj, L"SerialNumber", wSn))
                {
                    std::string sn = Trim(WideToUtf8(wSn));
                    if (!sn.empty())
                    {
                        result = sn;
                        return false;
                    }
                }
            }
            return true;
        });
    }

    return result;
}

std::string GetSystemDiskModel()
{
    std::string result;

    WmiQuery q;
    if (q.IsValid())
    {
        q.ForEach(L"SELECT Model, Index FROM Win32_DiskDrive", [&](IWbemClassObject* obj) -> bool {
            std::wstring wIndex;
            std::string index = WmiQuery::GetString(obj, L"Index", wIndex)
                ? Trim(WideToUtf8(wIndex)) : std::string();

            if (index == "0")
            {
                std::wstring wModel;
                if (WmiQuery::GetString(obj, L"Model", wModel))
                {
                    std::string model = Trim(WideToUtf8(wModel));
                    if (!model.empty())
                    {
                        result = model;
                        return false;
                    }
                }
            }
            return true;
        });
    }

    return result.empty() ? "未知" : result;
}

std::string GetOsVersion()
{
    std::string result;

    WmiQuery q;
    if (q.IsValid())
    {
        q.ForEach(L"SELECT Caption, Version FROM Win32_OperatingSystem", [&](IWbemClassObject* obj) -> bool {
            std::wstring wCaption, wVersion;
            std::string caption = WmiQuery::GetString(obj, L"Caption", wCaption)
                ? Trim(WideToUtf8(wCaption)) : std::string();
            std::string version = WmiQuery::GetString(obj, L"Version", wVersion)
                ? Trim(WideToUtf8(wVersion)) : std::string();

            result = Trim(caption + " " + version);
            return false;
        });
    }

    if (!result.empty())
        return result;

    // 回退：等价于 C# 的 Environment.OSVersion.ToString()
    // 使用 RtlGetVersion 而非 GetVersionEx（后者在 Win8.1+ 会因缺少清单而返回错误版本）
    typedef struct _PS_OSVERSIONINFOW {
        ULONG dwOSVersionInfoSize;
        ULONG dwMajorVersion;
        ULONG dwMinorVersion;
        ULONG dwBuildNumber;
        ULONG dwPlatformId;
        WCHAR szCSDVersion[128];
    } PS_OSVERSIONINFOW;

    typedef LONG(WINAPI* RtlGetVersionFn)(PS_OSVERSIONINFOW*);
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr)
    {
        RtlGetVersionFn fn = (RtlGetVersionFn)::GetProcAddress(ntdll, "RtlGetVersion");
        if (fn != nullptr)
        {
            PS_OSVERSIONINFOW vi = { 0 };
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (fn(&vi) == 0)
            {
                char buf[128];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                            "Microsoft Windows NT %lu.%lu.%lu.0",
                            vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
                return buf;
            }
        }
    }

    return "Microsoft Windows";
}

std::string GetDotNetVersion()
{
    // 原生 C++ 版没有 CLR。协议字段名保持 dotnet_version 不变（服务端按原样存储显示），
    // 取值改为标识本机运行库，便于管理员区分客户端类型。
    return "Native-C++ " + std::string(BuildConfig::VersionA);
}

std::map<std::string, std::string> GetHardwareInfo(const std::string& machineCode)
{
    std::map<std::string, std::string> info;
    info["machine_code"] = machineCode.empty() ? GenerateMachineCode() : machineCode;
    info["cpu_id"] = GetCpuId();
    info["cpu_name"] = GetCpuName();
    info["motherboard_sn"] = GetMotherboardSerial();
    info["motherboard_model"] = GetMotherboardModel();
    // 加权指纹用的稳定因子（服务端在机器码变化时靠它们认机器）
    info["machine_uuid"] = GetMotherboardUuid();
    info["bios_sn"] = GetBiosSerial();
    info["disk_sn"] = GetSystemDiskSerial();
    info["disk_model"] = GetSystemDiskModel();
    info["mac_address"] = GetPhysicalMac();
    info["hostname"] = WideToUtf8([]() -> std::wstring {
        wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
        DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
        ::GetComputerNameW(buf, &len);
        return std::wstring(buf, len);
    }());
    info["os_version"] = GetOsVersion();
    info["dotnet_version"] = GetDotNetVersion();
    return info;
}

}
}
