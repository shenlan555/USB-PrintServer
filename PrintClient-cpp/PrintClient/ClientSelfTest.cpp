#include "pch.h"
#include "ClientSelfTest.h"

#include <string>
#include <vector>
#include <cstdarg>

#include "DriverDownloader.h"
#include "FileUtil.h"
#include "PrinterDriverInstaller.h"
#include "Program.h"
#include "Wmi.h"
#ifdef _DEBUG

namespace ps
{

namespace
{

// 表格列宽（仅用于命令行自检报告，非界面元素）
std::string PadTo(const std::string& text, size_t width)
{
    std::string result = text;
    if (result.size() >= width)
        return result;

    result.append(width - result.size(), ' ');
    return result;
}

std::string IntToText(long long value)
{
    char buffer[32] = { 0 };
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%lld", value);
    return buffer;
}

} // namespace

int RunClientSelfTest(const ClientSelfTestOptions& options)
{
    std::string report;
    int failed = 0;

    const std::wstring& server = options.server;
    const int apiPort = options.apiPort;
    const int printerIndex = options.printerIndex;
    const std::wstring& reportPath = options.reportPath;

    auto append = [&report](const char* fmt, ...) {
        char buffer[4096];
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
        va_end(args);
        report += buffer;
    };

    auto check = [&](const char* name, bool pass, const std::string& detail = std::string()) {
        if (!pass)
            ++failed;
        append("[%s] %s%s%s\r\n", pass ? "PASS" : "FAIL", name,
               detail.empty() ? "" : " -> ", detail.c_str());
    };

    std::string serverUtf8 = WideToUtf8(server);

    SYSTEMTIME now;
    ::GetLocalTime(&now);

    append("网络打印客户端 命令行自检 (/clienttest)\r\n");
    append("服务器: %s:%d\r\n", serverUtf8.c_str(), apiPort);
    append("打印机索引: %d\r\n", printerIndex);
    append("本地时间: %04d-%02d-%02d %02d:%02d:%02d\r\n", now.wYear, now.wMonth, now.wDay,
           now.wHour, now.wMinute, now.wSecond);
    append("本机可执行文件: %s\r\n", WideToUtf8(ProgramHost::GetExecutablePath()).c_str());
    append("\r\n");

    DriverDownloader downloader(serverUtf8, apiPort);

    // ---- 1. 连接测试 ----
    bool connected = downloader.TestConnection();
    check("连接测试 (/api/test)", connected);
    if (!connected)
    {
        append("\r\n无法连接服务器，后续步骤中止。\r\n");
        append("\r\n=========== 结果: %s (失败 %d 项) ===========\r\n",
               failed == 0 ? "全部通过" : "存在失败", failed);
        WriteFileTextUtf8Bom(reportPath, report);
        return failed;
    }

    // ---- 2. 获取打印机信息 ----
    ServerPrinterInfo info;
    std::string infoError;
    bool infoOk = downloader.GetPrinterInfo(info, infoError);

    check("获取打印机信息 (/api/info)", infoOk, infoError);
    if (!infoOk)
    {
        append("\r\n=========== 结果: %s (失败 %d 项) ===========\r\n",
               failed == 0 ? "全部通过" : "存在失败", failed);
        WriteFileTextUtf8Bom(reportPath, report);
        return failed;
    }

    append("\r\n---- 服务器信息 ----\r\n");
    append("主机名: %s\r\n", info.hostname.c_str());
    append("版本: %s\r\n", info.version.c_str());
    append("API端口: %d\r\n", info.apiPort);
    append("打印机数量: %d\r\n", info.printerCount);
    append("旧版API字段 printer.name: %s\r\n", info.printerName.c_str());
    append("\r\n---- 打印机列表 (%d 台) ----\r\n", (int)info.printers.size());

    append("%s%s%s%s%s%s%s\r\n",
           PadTo("索引", 6).c_str(), PadTo("名称", 34).c_str(), PadTo("端口", 8).c_str(),
           PadTo("驱动名", 34).c_str(), PadTo("版本", 14).c_str(), PadTo("厂商", 20).c_str(),
           PadTo("驱动文件数", 12).c_str());

    for (const PrinterItemInfo& item : info.printers)
    {
        append("%s%s%s%s%s%s%s\r\n",
               PadTo(IntToText(item.index), 6).c_str(),
               PadTo(item.name, 34).c_str(),
               PadTo(IntToText(item.port), 8).c_str(),
               PadTo(item.driverName, 34).c_str(),
               PadTo(item.driverVersion, 14).c_str(),
               PadTo(item.manufacturer, 20).c_str(),
               PadTo(IntToText(item.driverFileCount), 12).c_str());
        append("      驱动日期: %s | 驱动包大小: %s | 可用: %s\r\n",
               item.driverDate.c_str(),
               item.driverSize.empty() ? "-" : item.driverSize.c_str(),
               item.hasDriver ? "是" : "否");
    }

    append("[INFO] 打印机列表非空: %s\r\n", info.printers.empty() ? "否" : "是");

    // 安装模式所需上下文（在下面的下载/解压块中填充）
    bool installContextReady = false;
    std::string installExtractDir;
    std::string installDriverName;
    std::string installDefaultName;
    int installTcpPort = 0;

    // ---- 3. 下载驱动包 ----
    if (info.printers.empty())
    {
        append("[SKIP] 无打印机，跳过驱动下载/解压\r\n");
    }
    else
    {
        int index = printerIndex;
        if (index < 0 || index >= (int)info.printers.size())
        {
            append("[WARN] 指定的打印机索引 %d 超出范围，改用 0\r\n", index);
            index = 0;
        }

        const PrinterItemInfo& chosen = info.printers[(size_t)index];
        append("\r\n选择打印机: [%d] %s (hasDriver=%s)\r\n", index, chosen.name.c_str(),
               chosen.hasDriver ? "true" : "false");

        // 安装模式需要的信息（与驱动是否下载成功无关的先填好）
        installDriverName = chosen.driverName;
        installDefaultName = TrimAscii(chosen.name) + "(共享)";
        installTcpPort = chosen.port;

        std::wstring tempDir = PathCombine(GetTempDirectory(), L"PrintClient_Driver");
        if (DirectoryExists(tempDir))
            RemoveDirectoryTree(tempDir);
        EnsureDirectory(tempDir);

        if (!chosen.hasDriver)
        {
            append("[SKIP] 该打印机没有驱动包，跳过下载/解压\r\n");
        }
        else
        {
            int lastPercent = -1;
            std::string zipPath;
            std::string downloadError;

            bool downloaded = downloader.DownloadDriver(
                WideToUtf8(tempDir), index,
                [&lastPercent](int percent) { lastPercent = percent; },
                zipPath, downloadError);

            check("下载驱动包 (/api/driver)", downloaded, downloadError);
            if (downloaded)
            {
                long long size = GetFileSize(Utf8ToWide(zipPath));
                append("      压缩包: %s (%s, 最后进度 %d%%)\r\n", zipPath.c_str(),
                       FormatSize(size).c_str(), lastPercent);

                // ---- 4. 解压 ----
                std::wstring extractDir = PathCombine(tempDir, L"driver");
                std::string extractError;
                bool extracted = DriverDownloader::ExtractDriver(zipPath, WideToUtf8(extractDir),
                                                                 extractError);
                check("解压驱动包", extracted, extractError);

                if (extracted)
                {
                    std::vector<std::wstring> files;
                    EnumerateFilesRecursive(extractDir, L"*.*", files);

                    std::vector<std::wstring> infFiles;
                    EnumerateFilesRecursive(extractDir, L"*.inf", infFiles);

                    append("      解压目录: %s\r\n", WideToUtf8(extractDir).c_str());
                    append("      解压文件数: %d\r\n", (int)files.size());
                    check("解压后存在文件", !files.empty(), IntToText((long long)files.size()) + " 个文件");
                    check("解压后有 INF 文件", !infFiles.empty(),
                          infFiles.empty() ? std::string() : WideToUtf8(GetFileName(infFiles[0])));

                    append("---- 解压文件清单（前 30 个）----\r\n");
                    int shown = 0;
                    for (const std::wstring& file : files)
                    {
                        if (shown++ >= 30)
                            break;
                        append("  %s\r\n", WideToUtf8(file).c_str());
                    }

                    installExtractDir = WideToUtf8(extractDir);
                    installContextReady = true;

                    if (!options.install)
                    {
                        // 仅自检时回收临时目录（对应 C# 解压后仍会重建）
                        RemoveDirectoryTree(tempDir);
                    }
                    else
                    {
                        append("      驱动目录（安装模式保留，便于排查）: %s\r\n",
                               installExtractDir.c_str());
                    }
                }
            }
        }
    }

    // ---- 5. 真实安装路径 + 独立校验 + 清理（仅 --install）----
    if (options.install)
    {
        // PowerShell 单引号字符串转义：' → ''
        auto quoteSingle = [](const std::wstring& text) {
            std::wstring quoted;
            for (wchar_t ch : text)
            {
                if (ch == L'\'')
                    quoted += L"''";
                else
                    quoted.push_back(ch);
            }
            return quoted;
        };

        // 与界面安装完全相同的调用参数：connectAddress = 命令行里的服务器地址，
        // 安装名默认 "<打印机名>(共享)"（界面里"安装名称"框的默认值）。
        std::string installName =
            options.installName.empty() ? installDefaultName : WideToUtf8(options.installName);
        std::string connectAddress = serverUtf8;
        std::string expectedPort =
            PrinterDriverInstaller::BuildPortName(connectAddress, installTcpPort);

        append("\r\n---- 真实安装路径 (PrinterDriverInstaller::InstallPrinter) ----\r\n");
        append("安装名称: %s\r\n", installName.c_str());
        append("连接地址: %s | 打印机端口(TCP): %d | 驱动名: %s\r\n", connectAddress.c_str(),
               installTcpPort, installDriverName.c_str());
        append("预计端口名: %s\r\n", expectedPort.c_str());

        if (!installContextReady)
        {
            ++failed;
            append("[FAIL] 安装打印机 -> 前置步骤未就绪（没有可用的驱动目录）\r\n");
        }
        else
        {
            append("驱动目录: %s\r\n", installExtractDir.c_str());

            ps::InstallResult result = PrinterDriverInstaller::InstallPrinter(
                installExtractDir, connectAddress, installTcpPort, installName, installDriverName,
                [&append](const std::string& message) {
                    append("      [进度] %s\r\n", message.c_str());
                });

            check("安装打印机 (InstallPrinter 返回 Success)", result.success, result.message);
            append("      Success: %s\r\n", result.success ? "true" : "false");
            append("      Message: %s\r\n", result.message.c_str());
            append("      PrinterName: %s\r\n", result.printerName.c_str());
            append("      PortName: %s\r\n", result.portName.c_str());
            append("      DriverName: %s\r\n", result.driverName.c_str());

            int total = (int)result.steps.size();
            append("\r\n---- 安装步骤 (%d 条，文本与界面运行日志一致) ----\r\n", total);

            int stepIndex = 0;
            for (const std::string& step : result.steps)
            {
                ++stepIndex;

                const char* tag = "INFO";
                if (step.find("❌") != std::string::npos)
                {
                    tag = "FAIL";
                    ++failed;
                }
                else if (step.find("⚠") != std::string::npos)
                {
                    tag = "WARN";
                }
                else if (step.find("✅") != std::string::npos ||
                         step.find("🎉") != std::string::npos)
                {
                    tag = "PASS";
                }

                append("[%s] 步骤 %d/%d: %s\r\n", tag, stepIndex, total, step.c_str());
            }

            if (!result.portName.empty())
                expectedPort = result.portName;
            if (!result.printerName.empty())
                installName = result.printerName;
        }

        // ---- 6. 独立校验（打印后台 API：OpenPrinter / EnumPrinters / EnumPorts）----
        append("\r\n---- 独立校验（不依赖安装过程内部的 WMI 检查）----\r\n");

        bool printerExists = PrinterDriverInstaller::PrinterExistsByApi(installName);
        check("独立校验: OpenPrinter/EnumPrinters 找到打印机", printerExists, installName);

        std::string boundPort;
        if (PrinterDriverInstaller::GetPrinterPortNameByApi(installName, boundPort))
            append("      打印机实际绑定的端口: %s\r\n", boundPort.c_str());

        bool portExists = PrinterDriverInstaller::PortExistsByApi(expectedPort);
        check("独立校验: EnumPorts 找到端口", portExists, expectedPort);
        append("      创建的端口名: %s\r\n", expectedPort.c_str());

        // ---- 7. 清理：删除打印机队列与端口，驱动库保持不动 ----
        append("\r\n---- 清理: 删除打印机队列与端口（不卸载驱动包）----\r\n");

        std::vector<std::string> removedItems;

        // 清理前是否存在（只有真正执行过删除且现在确实消失了才计入"已删除"）
        bool printerExisted = printerExists;
        bool portExisted = portExists;

        // 7.1 打印机队列：rundll32 printui.dll（与安装时的 printui 技术同源）
        if (printerExists)
        {
            std::wstring args = L"printui.dll,PrintUIEntry /dl /n \"" + Utf8ToWide(installName) +
                                L"\" /q";

            PrinterDriverInstaller::CommandResult command;
            bool started =
                PrinterDriverInstaller::RunCommand(L"rundll32.exe", args, 60000, command);
            // rundll32 的退出码并不可靠：只要事后确实删掉了就算成功
            bool gone = !PrinterDriverInstaller::PrinterExistsByApi(installName);
            bool ok = (started && command.exitCode == 0) || gone;
            if (!ok)
                ++failed;

            append("[%s] 清理打印机: rundll32 printui.dll,PrintUIEntry /dl /n \"%s\" /q -> "
                   "启动=%s 退出码=%lu 已删除=%s\r\n",
                   ok ? "PASS" : "FAIL", installName.c_str(), started ? "true" : "false",
                   (unsigned long)command.exitCode, gone ? "true" : "false");

            std::string text = TrimAscii(command.output + command.error);
            if (!text.empty())
                append("      输出: %s\r\n", text.c_str());
        }
        else
        {
            append("[INFO] 打印机队列不存在，跳过删除\r\n");
        }

        // 7.2 仍存在时回退 PowerShell Remove-Printer（与安装的 Add-Printer 同源）
        if (PrinterDriverInstaller::PrinterExistsByApi(installName))
        {
            std::wstring commandText =
                L"Remove-Printer -Name '" + quoteSingle(Utf8ToWide(installName)) +
                L"' -ErrorAction Stop";
            std::wstring args = L"-NoProfile -Command \"" + commandText + L"\"";

            PrinterDriverInstaller::CommandResult command;
            bool started =
                PrinterDriverInstaller::RunCommand(L"powershell.exe", args, 60000, command);
            bool gone = !PrinterDriverInstaller::PrinterExistsByApi(installName);
            bool ok = (started && command.exitCode == 0) || gone;
            if (!ok)
                ++failed;

            append("[%s] 清理打印机(回退): powershell -NoProfile -Command \"%s\" -> "
                   "启动=%s 退出码=%lu 已删除=%s\r\n",
                   ok ? "PASS" : "FAIL", WideToUtf8(commandText).c_str(), started ? "true" : "false",
                   (unsigned long)command.exitCode, gone ? "true" : "false");

            std::string text = TrimAscii(command.output + command.error);
            if (!text.empty())
                append("      输出: %s\r\n", text.c_str());
        }

        if (printerExisted && !PrinterDriverInstaller::PrinterExistsByApi(installName))
            removedItems.push_back("打印机队列: " + installName);

        check("清理后打印机已删除", !PrinterDriverInstaller::PrinterExistsByApi(installName),
              installName);

        // 7.3 端口：WMI Win32_TCPIPPrinterPort（与创建端口的 WMI 路径同一机制）
        if (portExists)
        {
            std::string wmiError;
            bool wmiOk = PrinterDriverInstaller::DeleteTcpIpPrinterPortViaWmi(expectedPort, wmiError);
            bool gone = !PrinterDriverInstaller::PortExistsByApi(expectedPort);
            bool ok = wmiOk || gone;
            if (!ok)
                ++failed;

            append("[%s] 清理端口: WMI Win32_TCPIPPrinterPort DeleteInstance(\"%s\") -> "
                   "已删除=%s%s\r\n",
                   ok ? "PASS" : "FAIL", expectedPort.c_str(), gone ? "true" : "false",
                   (wmiOk || wmiError.empty()) ? "" : (" -> " + wmiError).c_str());
        }
        else
        {
            append("[INFO] 端口不存在，跳过删除\r\n");
        }

        // 7.4 仍存在时回退 PowerShell Remove-PrinterPort（与 Add-PrinterPort 同源）
        if (PrinterDriverInstaller::PortExistsByApi(expectedPort))
        {
            std::wstring commandText =
                L"Remove-PrinterPort -Name '" + quoteSingle(Utf8ToWide(expectedPort)) +
                L"' -ErrorAction Stop";
            std::wstring args = L"-NoProfile -Command \"" + commandText + L"\"";

            PrinterDriverInstaller::CommandResult command;
            bool started =
                PrinterDriverInstaller::RunCommand(L"powershell.exe", args, 60000, command);
            bool gone = !PrinterDriverInstaller::PortExistsByApi(expectedPort);
            bool ok = (started && command.exitCode == 0) || gone;
            if (!ok)
                ++failed;

            append("[%s] 清理端口(回退): powershell -NoProfile -Command \"%s\" -> "
                   "启动=%s 退出码=%lu 已删除=%s\r\n",
                   ok ? "PASS" : "FAIL", WideToUtf8(commandText).c_str(), started ? "true" : "false",
                   (unsigned long)command.exitCode, gone ? "true" : "false");

            std::string text = TrimAscii(command.output + command.error);
            if (!text.empty())
                append("      输出: %s\r\n", text.c_str());
        }

        if (portExisted && !PrinterDriverInstaller::PortExistsByApi(expectedPort))
            removedItems.push_back("打印端口: " + expectedPort);

        check("清理后端口已删除", !PrinterDriverInstaller::PortExistsByApi(expectedPort),
              expectedPort);

        // 7.5 端口注册表项（ConfigurePortBidirectional 会写入，端口删除后本应消失）
        {
            bool keyRemoved = false;
            if (PrinterDriverInstaller::RemovePortRegistryKey(expectedPort, keyRemoved))
            {
                append("[PASS] 清理端口注册表项 HKLM\\SYSTEM\\...\\Standard TCP/IP "
                       "Port\\Ports\\%s: %s\r\n",
                       expectedPort.c_str(), keyRemoved ? "已删除" : "无需清理（不存在）");

                if (keyRemoved)
                    removedItems.push_back("端口注册表项: " + expectedPort);
            }
            else
            {
                ++failed;
                append("[FAIL] 清理端口注册表项 HKLM\\SYSTEM\\...\\Ports\\%s 失败\r\n",
                       expectedPort.c_str());
            }
        }

        // 7.6 汇总
        append("\r\n---- 清理结果 ----\r\n");
        append("共删除 %d 项:\r\n", (int)removedItems.size());
        if (removedItems.empty())
            append("  （无需删除，机器保持原样）\r\n");
        for (const std::string& item : removedItems)
            append("  - %s\r\n", item.c_str());

        append("驱动包 / 驱动库: 未卸载（保持原样）\r\n");
        if (installContextReady)
            append("驱动目录: %s（保留）\r\n", installExtractDir.c_str());
    }

    append("\r\n=========== 结果: %s (失败 %d 项) ===========\r\n",
           failed == 0 ? "全部通过" : "存在失败", failed);

    // 带 BOM 写入，便于记事本正确显示中文
    WriteFileTextUtf8Bom(reportPath, report);

    return failed;
}

} // namespace ps
#endif // _DEBUG
