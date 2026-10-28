#pragma once

// 打印机驱动打包（对应 C# DriverPackager.cs）
//
// v1.4.1 起改为“以 Windows 已安装的驱动包为单位”抓取，不再按驱动名做模糊匹配：
//   路线A：定位 DriverStore 包目录（自洽完整，含 原始INF + .cat + 全部依赖），整目录打包
//   路线B：DriverStore 缺失时，按注册表 Version-3/Version-4 的精确项解析文件清单
// 两条路线都会：保留目录结构、生成 manifest.json（含 SHA256 清单）、做完整性门禁。
//
// ZIP 使用内联的 miniz（third_party/miniz，公有领域/MIT）实现 deflate 压缩，
// 不引入任何运行时依赖。

#include <string>
#include <vector>

namespace ps
{

// 对应 C# DriverPackager.PrinterDriverInfo
struct PrinterDriverInfo
{
    std::string driverName;
    std::string driverVersion;
    std::string driverDate;
    std::string manufacturer;
    std::string infPath;            // oemXX.inf
    std::string driverStorePath;    // 命中的 DriverStore 包目录（路线A）
    std::string architecture;       // amd64 / x86
    std::string packageSource;      // driverstore / registry
    bool complete = false;          // INF/.cat/依赖文件是否齐全
    int missingFileCount = 0;
    bool hasDriverFiles = false;
    int driverFileCount = 0;
    long long driverPackageSize = 0;
    std::vector<std::wstring> driverFiles;
};

namespace DriverPackager
{
    // 包内清单文件名（客户端据此唯一定位 INF 并校验 SHA256）
    extern const char* const kManifestFileName;

    // 获取打印机驱动信息
    PrinterDriverInfo GetPrinterDriverInfo(const std::string& printerNameUtf8);

    // 打包驱动文件为 ZIP；失败返回空
    std::vector<unsigned char> PackageDriverFiles(const std::string& printerNameUtf8);

    // 打包指定目录为 ZIP（entry 名为相对路径）
    std::vector<unsigned char> ZipDirectory(const std::wstring& dirPath);
}

}
