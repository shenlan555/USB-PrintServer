#pragma once

// 打印机驱动安装器（对应 C# PrinterDriverInstaller.cs）
//
// 在客户端安装打印机驱动并创建网络打印机。
// 步骤、回退链、提示文本与 C# 逐行对应。

#include <string>
#include <vector>
#include <functional>

namespace ps
{

// 对应 PrinterDriverInstaller.InstallResult
struct InstallResult
{
    bool success = false;
    std::string message;
    std::string printerName;
    std::string portName;
    std::string driverName;
    std::vector<std::string> steps;
};

using InstallProgressFn = std::function<void(const std::string&)>;

// 外部命令执行结果（复用安装过程内部同一套 RunProcess 实现）
namespace PrinterDriverInstaller
{

struct CommandResult
{
    bool started = false;
    bool timedOut = false;
    DWORD exitCode = 0;
    std::string output;
    std::string error;
};

// 对应 InstallPrinter(driverDirectory, serverHost, serverPort, printerName, driverName, progress)
InstallResult InstallPrinter(const std::string& driverDirectory,
                             const std::string& serverHost,
                             int serverPort,
                             const std::string& printerName,
                             const std::string& driverName,
                             const InstallProgressFn& progress);

// 对应 CreateTcpIpPort 中使用的端口名规则 PS_<host>_<port>
std::string BuildPortName(const std::string& serverHost, int serverPort);

// ===========================================================================
// IPP 免驱安装
// ===========================================================================
//
// 用 Windows 自带的 "Microsoft IPP Class Driver" + 一个内容为 IPP URL 的端口
// 直接建出打印队列，**完全不下载、不安装厂商驱动**，因此也不需要为装驱动提权。
//
// 为什么值得单独做一条路：
//   1) 客户端的常规安装要"下载驱动包 → 装驱动 → 建端口 → 建队列"，链路长且
//      每一步都可能被权限/签名/版本兼容卡住；
//   2) 免驱路径只需要系统自带的类驱动，Windows 10 1809+ 都有；
//   3) 打印时由服务端负责把 Java... 即客户端发来的 PDF 渲染后交给本机驱动。
//
// 注意：本函数内部用 rundll32 建队列，而 rundll32 的退出码恒为 0，
//       因此一律以"回读系统里是否真的出现了该打印机/端口"为准。
//
// printerIndex 是"服务端上的第几台打印机"：
//   0 → http://<host>:631/ipp/print
//   n → http://<host>:631/ipp/print/<n>
// 必须与服务端的 PrinterUriFor 严格一致，否则会把 1 号打印机的作业发到 0 号。
InstallResult InstallPrinterViaIpp(const std::string& serverHost, int ippPort, int printerIndex,
                                   const std::string& printerName,
                                   const InstallProgressFn& progress);

// ---- 供命令行自检 / 清理复用（安装路径本身不使用） ----

// 执行外部命令并取回退出码与输出（与安装过程调用 rundll32/pnputil/powershell 时完全相同）
bool RunCommand(const std::wstring& fileName, const std::wstring& arguments, DWORD timeoutMs,
                CommandResult& result);

// 删除 TCP/IP 打印端口：WMI Win32_TCPIPPrinterPort（与创建端口时的 WMI 路径同一机制）
bool DeleteTcpIpPrinterPortViaWmi(const std::string& portName, std::string& error);

// 删除端口注册表项 HKLM\SYSTEM\...\Standard TCP/IP Port\Ports\<port>（已不存在返回 true）
bool RemovePortRegistryKey(const std::string& portName, bool& removed);

// 独立的存在性检查：直接走打印后台 API（OpenPrinter / EnumPrinters / EnumPorts），
// 与安装过程内部使用的 WMI 检查相互独立。
bool PrinterExistsByApi(const std::string& printerName);
bool GetPrinterPortNameByApi(const std::string& printerName, std::string& portName);
bool PortExistsByApi(const std::string& portName);

} // namespace PrinterDriverInstaller
} // namespace ps
