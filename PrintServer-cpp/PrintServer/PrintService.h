#pragma once

// 打印服务（对应 C# PrintServerService.cs）
//
// 包含：打印机配置读取、网络信息助手、RAW 直通打印、单台打印机的 TCP 打印服务。

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>

namespace ps
{

// 对应 C# PrinterConfig
struct PrinterConfig
{
    std::string name;
    int port = 9100;
    bool enabled = true;
    int index = 0;
};

// 对应 C# ConfigurationHelper
namespace ConfigHelper
{
    // 读取 appSettings 项（沿用 Config::GetSetting）
    std::string GetSetting(const std::string& key, const std::string& defaultValue);

    // 配置文件中的一个稳定槽位（对应 Printer<slot>_Name/Port/Enabled 这组键）。
    // 槽位一旦分配就固定：取消勾选只把 Enabled 置 false，不会让后面打印机的
    // 键名整体前移，因此端口、IPP 的 /ipp/print/N 都能长期保持稳定。
    struct PrinterSlot
    {
        int slot = 0;
        std::string name;
        int port = 9100;
        bool enabled = false;
    };

    // 读取全部槽位（含未勾选的），供 GUI 恢复勾选状态/端口使用
    std::vector<PrinterSlot> GetPrinterSlots();

    // 获取所有启用的打印机配置
    // PrinterCount<=0 时回退到单打印机模式（PrinterName/Port）
    std::vector<PrinterConfig> GetPrinterConfigs();
}

// 对应 C# NetworkHelper
namespace NetworkHelper
{
    // WinSock 初始化（进程内只需一次）
    // C# 侧由 CLR 隐式完成；原生侧不显式调用时 gethostname/getaddrinfo 会返回
    // WSANOTINITIALISED，地址查询将退化为 127.0.0.1
    void EnsureWinsock();
    // 本机 IPv4 地址列表
    std::vector<std::string> GetAllNetworkAddresses();
    // 反查主机名（失败时返回原 IP）
    std::string GetHostName(const std::string& ipAddress);
    // 解析主机名到 IPv4 列表
    bool ResolveHostName(const std::string& hostName, std::vector<std::string>& ipAddresses);
}

// 对应 C# RawPrinterHelper
namespace RawPrinter
{
    // RAW 直通打印；失败返回 false（与 C# 一致，不抛异常）
    bool SendBytesToPrinter(const std::string& printerNameUtf8,
                            const std::vector<unsigned char>& data);

    // 从文件指定区间流式打印（IPP 用）：
    //   - 分块读写，避免大文档整包进内存
    //   - shouldCancel 非空时，每块写完后回调；返回 true 则中止作业
    //   - 全部成功返回 true；被取消返回 false 且 canceled 置 true
    bool SendFileRangeToPrinter(const std::string& printerNameUtf8,
                                const std::wstring& filePath,
                                long long offset, long long length,
                                const std::function<bool()>& shouldCancel,
                                bool& canceled);
}

// 对应 C# PrintServerService：单台打印机的 TCP RAW 打印服务
class TcpPrintServer
{
public:
    TcpPrintServer(const std::string& printerName, int port);
    ~TcpPrintServer();

    TcpPrintServer(const TcpPrintServer&) = delete;
    TcpPrintServer& operator=(const TcpPrintServer&) = delete;

    void Start();
    void Stop();

    bool IsRunning() const { return m_running.load(); }

    // 对应 C# 的 OnLog / OnClientCountChanged 事件
    std::function<void(const std::string&)> OnLog;
    std::function<void(int)> OnClientCountChanged;

private:
    void ListenLoop();
    void HandleClient(uintptr_t clientSocket);
    void RemoveActiveClient(uintptr_t clientSocket);   // 关闭套接字并从活动列表移除
    bool PrintTempFile(const std::wstring& filePath);
    void Log(const std::string& message);

    std::string m_printerName;
    int m_port;
    std::atomic<bool> m_running;
    uintptr_t m_listenSocket;                 // SOCKET
    std::thread m_listenThread;

    std::mutex m_clientsMutex;
    std::vector<uintptr_t> m_activeClients;
};

// 发送 UTF-8 消息到 Windows 事件日志（对应 C# EventLog 写入 Application 日志）
void WriteEventLog(const std::string& message, bool isError);

} // namespace ps
