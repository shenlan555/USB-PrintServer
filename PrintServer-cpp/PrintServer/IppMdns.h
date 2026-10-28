#pragma once

// mDNS / DNS-SD 服务广播（RFC 6762 / 6763）
//
// 目的：让打印机自动出现在客户端「添加打印机」列表里，无需手输 URL。
//
// 为什么必须要它：
//   1) 我们实测发现，Windows 这一代真正可靠的 IPP 安装路径是"发现式安装"
//      （自动发现后由系统建好端口），而手输 URL 走的是旧 inetpp 路径，
//      队列虽能建出来，但打印时在本地就失败。本机注册表里 IPP 端口是挂在
//      WSD Port 监视器的 Adapters\IPP 下，也印证了这一点。
//   2) macOS / iOS(AirPrint) / Android(Mopria) / Linux(CUPS) 全靠 mDNS 发现。
//
// 广播的服务：
//   _ipp._tcp.local              每个打印机一条；另加 _universal._sub 子类型
//                                （IPP Everywhere / AirPrint 靠子类型订阅发现）
//   _pdl-datastream._tcp.local   对应 RAW 端口（9100+i），供只支持 RAW 的客户端

#include <string>
#include <vector>

namespace ps
{
namespace IppMdns
{

struct PrinterService
{
    std::string name;              // 显示名（打印机名）
    std::string makeAndModel;      // 型号
    std::string uuid;              // urn:uuid:...
    std::string pdl;               // "application/pdf,application/octet-stream"
    int ippPort = 631;             // IPP 端口
    std::string ippPath;           // TXT 的 rp 值，如 "ipp/print" / "ipp/print/1"
    int rawPort = 0;               // RAW 端口；<=0 表示不广播 RAW
    bool color = false;
    bool duplex = false;
};

// 启动广播。address 为本机对外 IPv4，hostName 为本机名（用作 SRV 目标 <host>.local）
bool Start(const std::string& hostName, const std::string& address,
           const std::vector<PrinterService>& services, std::string& error);

void Stop();

bool IsRunning();

} // namespace IppMdns
} // namespace ps
