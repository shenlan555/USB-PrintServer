#pragma once

// 驱动下载器（对应 C# DriverDownloader.cs）
//
// 从打印服务器获取打印机列表和驱动文件，支持多打印机模式。

#include <string>
#include <vector>
#include <functional>

namespace ps
{

// 对应 DriverDownloader.ServerPrinterInfo
struct ServerPrinterInfo;

// 对应 DriverDownloader.PrinterItemInfo
struct PrinterItemInfo
{
    int index = 0;
    std::string name;
    int port = 0;
    std::string driverName;
    std::string driverVersion;
    std::string driverDate;
    std::string manufacturer;
    bool hasDriver = false;
    int driverFileCount = 0;
    std::string driverSize;
};

struct ServerPrinterInfo
{
    std::string hostname;
    std::string version;
    int tcpPort = 0;
    int apiPort = 0;
    int printerCount = 0;
    std::string printerName;
    std::string driverName;
    std::string driverVersion;
    std::string driverDate;
    std::string manufacturer;
    bool hasDriver = false;
    int driverFileCount = 0;
    std::string driverSize;

    std::vector<PrinterItemInfo> printers;
};

// 清单中的单个文件条目
struct ManifestFileEntry
{
    std::string path;
    long long size = 0;
    std::string sha256;
};

// 服务端驱动包清单（包内 _printserver_driver.json）
//
// v1.4.1 起服务端在驱动包内附带该清单，客户端据此：
//   1) 唯一定位 INF（不再“取第一个找到的 .inf”）
//   2) 采用服务端给出的精确驱动名
//   3) 逐文件校验 SHA256（校验截断/损坏/内容被替换）
struct DriverManifest
{
    bool present = false;
    std::string driverName;                  // 用于 Add-Printer 的驱动名
    std::string inf;                         // 包内 INF 相对路径（'/' 分隔）
    std::string architecture;                // amd64 / x86
    std::string source;                      // driverstore / registry
    std::string catalogFile;
    bool complete = true;                    // 服务端自检：INF 声明的文件是否齐全
    std::vector<std::string> missing;
    std::vector<std::string> infModels;
    std::vector<ManifestFileEntry> files;    // 逐文件 SHA256 清单
    std::string signature;                   // 清单的 RSA 签名（Base64）
    bool signaturePresent = false;
    bool signatureValid = false;             // 用内置公钥验签的结果
};

class DriverDownloader
{
public:
    DriverDownloader(const std::string& serverHost, int apiPort = 9180);

    // 对应 TestConnectionAsync()：GET /api/test，5 秒超时
    bool TestConnection();

    // 对应 GetPrinterInfoAsync()：GET /api/info，10 秒超时；失败时 error 为
    // "获取打印机信息失败: " + 原因
    bool GetPrinterInfo(ServerPrinterInfo& info, std::string& error);

    // 对应 DownloadDriverAsync()：GET /api/driver（id > 0 时附加 ?id=），5 分钟超时，
    // 8192 字节分块写盘并汇报百分比；失败时 error 为 "下载驱动失败: " + 原因
    bool DownloadDriver(const std::string& saveDirectory, int printerIndex,
                        const std::function<void(int)>& progress,
                        std::string& zipPath, std::string& error);

    // 对应静态方法 ExtractDriver()：先删除目标目录再用 miniz 解压；
    // 失败时 error 为 "解压驱动失败: " + 原因
    static bool ExtractDriver(const std::string& zipPath, const std::string& extractDir,
                              std::string& error);

    // 读取驱动包清单 <driverDirectory>/_printserver_driver.json；
    // 清单不存在时 present=false（兼容旧版服务端）
    static bool LoadManifest(const std::string& driverDirectory, DriverManifest& manifest);

    // 逐文件校验清单中的 SHA256；失败时 error 说明具体文件
    static bool VerifyManifestFiles(const std::string& driverDirectory,
                                    const DriverManifest& manifest, std::string& error);

    // 清单文件名（与 PrintServer 端 DriverPackager::kManifestFileName 保持一致）
    static const char* ManifestFileName();

    const std::string& BaseUrl() const { return m_serverBaseUrl; }

private:
    std::string m_serverBaseUrl;
};

} // namespace ps
