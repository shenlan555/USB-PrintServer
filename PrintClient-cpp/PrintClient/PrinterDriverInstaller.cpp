#include "pch.h"
#include "PrinterDriverInstaller.h"

#include <string>
#include <vector>
#include <thread>
#include <atomic>

#include <winspool.h>
#include <comdef.h>

#include "FileUtil.h"
#include "Wmi.h"
#include "DriverDownloader.h"

namespace ps
{
namespace PrinterDriverInstaller
{

namespace
{

// ===========================================================================
// Win32 常量
//
// APD_* 打印机驱动安装标志与 PRINTER_INFO_2 / DRIVER_INFO_3 结构均由
// winspool.h 提供，这里不再重复定义。
// ===========================================================================

// 对应 C# ManagementStatus.AlreadyExists
const HRESULT kManagementAlreadyExists = (HRESULT)0x80041019L;

void ReportProgress(const InstallProgressFn& progress, const std::string& message)
{
    if (progress)
        progress(message);
}

// ===========================================================================
// 子进程调用（对应 Process.Start + RedirectStandardOutput/Error）
// ===========================================================================

struct ProcessResult
{
    bool started = false;
    bool timedOut = false;
    DWORD exitCode = 0;
    std::string output;
    std::string error;
};

// 在 PATH 中解析可执行文件（对应 .NET 启动子进程时的查找顺序）
std::wstring ResolveExecutable(const std::wstring& fileName)
{
    wchar_t buffer[MAX_PATH * 2] = { 0 };
    DWORD length = ::SearchPathW(nullptr, fileName.c_str(), L".exe",
                                 _countof(buffer), buffer, nullptr);
    if (length > 0 && length < _countof(buffer))
        return buffer;

    return fileName;
}

struct PipeReaderContext
{
    HANDLE pipe = nullptr;
    std::vector<char> bytes;
};

void ReadPipeToEnd(PipeReaderContext* context)
{
    char buffer[4096];
    for (;;)
    {
        DWORD read = 0;
        if (!::ReadFile(context->pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0)
            break;

        context->bytes.insert(context->bytes.end(), buffer, buffer + read);
    }
}

bool RunProcess(const std::wstring& fileName, const std::wstring& arguments, DWORD timeoutMs,
                ProcessResult& result)
{
    result = ProcessResult();

    SECURITY_ATTRIBUTES attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    HANDLE errRead = nullptr;
    HANDLE errWrite = nullptr;

    if (!::CreatePipe(&outRead, &outWrite, &attributes, 0))
        return false;

    if (!::CreatePipe(&errRead, &errWrite, &attributes, 0))
    {
        ::CloseHandle(outRead);
        ::CloseHandle(outWrite);
        return false;
    }

    ::SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring application = ResolveExecutable(fileName);
    std::wstring commandLine = L"\"" + application + L"\" " + arguments;

    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = outWrite;
    startup.hStdError = errWrite;

    // C# 的 Process.Start 在未重定向 stdin 时会给子进程一个空输入；
    // 这里显式接上 NUL，避免 GUI 进程没有 stdin 时子进程拿到无效句柄。
    HANDLE nullInput = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &attributes, OPEN_EXISTING, 0, nullptr);
    startup.hStdInput = (nullInput != INVALID_HANDLE_VALUE)
                            ? nullInput
                            : ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof(process));

    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    BOOL created = ::CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                    CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);

    ::CloseHandle(outWrite);
    ::CloseHandle(errWrite);

    if (nullInput != INVALID_HANDLE_VALUE)
        ::CloseHandle(nullInput);

    if (!created)
    {
        ::CloseHandle(outRead);
        ::CloseHandle(errRead);
        return false;
    }

    result.started = true;

    PipeReaderContext outContext;
    PipeReaderContext errContext;
    outContext.pipe = outRead;
    errContext.pipe = errRead;

    std::thread outReader(ReadPipeToEnd, &outContext);
    std::thread errReader(ReadPipeToEnd, &errContext);

    DWORD wait = ::WaitForSingleObject(process.hProcess, timeoutMs);
    if (wait == WAIT_TIMEOUT)
    {
        // C# 在超时后仍会读到底（可能长时间阻塞）；这里终止子进程避免界面卡死
        result.timedOut = true;
        ::TerminateProcess(process.hProcess, (UINT)-1);
        ::WaitForSingleObject(process.hProcess, 5000);
    }

    DWORD exitCode = 0;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    result.exitCode = exitCode;

    outReader.join();
    errReader.join();

    // 对应 .NET RedirectStandardOutput 的默认解码（系统 ANSI 代码页）
    result.output = AnsiToUtf8(outContext.bytes);
    result.error = AnsiToUtf8(errContext.bytes);

    ::CloseHandle(outRead);
    ::CloseHandle(errRead);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);

    return true;
}

// ===========================================================================
// INF 处理
// ===========================================================================

// 对应 File.ReadAllText(infPath, Encoding.Unicode)
// StreamReader 默认会自动识别 BOM：UTF-8 BOM → UTF-8，UTF-16 BOM → UTF-16；
// 无 BOM 时按 Encoding.Unicode（UTF-16LE）解码。
std::wstring ReadInfText(const std::wstring& infPath)
{
    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(infPath, bytes) || bytes.empty())
        return std::wstring();

    size_t offset = 0;
    bool bigEndian = false;
    bool decodeUtf8 = false;

    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
    {
        offset = 3;
        decodeUtf8 = true;
    }
    else if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
    {
        offset = 2;
    }
    else if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF)
    {
        offset = 2;
        bigEndian = true;
    }

    if (decodeUtf8)
    {
        std::string utf8((const char*)bytes.data() + offset, bytes.size() - offset);
        return Utf8ToWide(utf8);
    }

    std::wstring text;
    text.reserve((bytes.size() - offset) / 2);

    for (size_t i = offset; i + 1 < bytes.size(); i += 2)
    {
        unsigned char low = bigEndian ? bytes[i + 1] : bytes[i];
        unsigned char high = bigEndian ? bytes[i] : bytes[i + 1];
        text.push_back((wchar_t)((unsigned)low | ((unsigned)high << 8)));
    }

    return text;
}

// 前置声明：FindInfFile 需要按驱动名匹配 INF
std::wstring ReadDriverNameFromInf(const std::wstring& infPath);

// 对应 PrinterDriverInstaller.FindInfFile
//
// v1.4.1：不再“取第一个找到的 .inf”。优先使用服务端清单指定的 INF，
// 其次挑选内容与期望驱动名一致的 INF，最后才兜底。
std::wstring FindInfFile(const std::wstring& directory,
                         const std::wstring& preferredInfName,
                         const std::string& expectedDriverName)
{
    std::vector<std::wstring> files;
    EnumerateFilesRecursive(directory, L"*.inf", files);
    if (files.empty())
        return std::wstring();

    auto fileNameOf = [](const std::wstring& path) {
        std::wstring name = GetFileName(path);
        for (wchar_t& ch : name)
            ch = (wchar_t)::towlower(ch);
        return name;
    };

    // 1) 清单指定的 INF 文件名（忽略其目录部分）
    if (!preferredInfName.empty())
    {
        std::wstring target = preferredInfName;
        size_t slash = target.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            target = target.substr(slash + 1);
        for (wchar_t& ch : target)
            ch = (wchar_t)::towlower(ch);

        for (const std::wstring& file : files)
        {
            if (fileNameOf(file) == target)
                return file;
        }
    }

    // 2) 按期望驱动名匹配 INF 内容
    if (!expectedDriverName.empty())
    {
        for (const std::wstring& file : files)
        {
            if (fileNameOf(file) == L"setup.inf")
                continue;

            std::wstring name = ReadDriverNameFromInf(file);
            if (!name.empty() && WideToUtf8(name) == expectedDriverName)
                return file;
        }
    }

    // 3) 兜底：第一个非 setup.inf
    for (const std::wstring& file : files)
    {
        if (fileNameOf(file) != L"setup.inf")
            return file;
    }

    return files[0];
}

// 对应 PrinterDriverInstaller.ReadDriverNameFromInf
std::wstring ReadDriverNameFromInf(const std::wstring& infPath)
{
    std::wstring content = ReadInfText(infPath);
    if (content.empty())
        return std::wstring();

    std::vector<std::wstring> lines;
    {
        std::wstring current;
        for (wchar_t ch : content)
        {
            if (ch == L'\r' || ch == L'\n')
            {
                if (!current.empty())
                    lines.push_back(current);
                current.clear();
            }
            else
            {
                current.push_back(ch);
            }
        }

        if (!current.empty())
            lines.push_back(current);
    }

    auto trim = [](const std::wstring& text) { return TrimWide(text); };
    auto containsIgnoreCase = [](const std::wstring& text, const wchar_t* needle) {
        std::wstring lower = text;
        for (wchar_t& ch : lower)
            ch = (wchar_t)::towlower(ch);
        return lower.find(needle) != std::wstring::npos;
    };

    // 策略1：在 Manufacturer 节下面找模型名
    bool inModelsSection = false;
    for (const std::wstring& line : lines)
    {
        std::wstring trimmed = trim(line);

        if (!trimmed.empty() && trimmed.front() == L'[' && trimmed.back() == L']')
        {
            std::wstring section = trimmed.substr(1, trimmed.size() - 2);
            while (!section.empty() && (section.front() == L'[' || section.front() == L']'))
                section.erase(section.begin());
            while (!section.empty() && (section.back() == L']' || section.back() == L'['))
                section.pop_back();

            if (containsIgnoreCase(section, L"nt") && !containsIgnoreCase(trimmed, L"strings"))
            {
                inModelsSection = true;
                continue;
            }

            if (containsIgnoreCase(trimmed, L"strings"))
            {
                inModelsSection = false;
                continue;
            }

            if (section.find(L'.') == std::wstring::npos)
                inModelsSection = false;

            continue;
        }

        if (!inModelsSection)
            continue;

        if (trimmed.empty() || trimmed.front() != L'"')
            continue;

        size_t eq = trimmed.find(L'=');
        if (eq == std::wstring::npos || eq == 0)
            continue;

        // 对应 trimmed.Substring(1, eqIdx - 1).Trim().Trim('"').Trim()
        std::wstring modelName = trimmed.substr(1, eq - 1);
        modelName = TrimWide(modelName);
        {
            while (!modelName.empty() && modelName.front() == L'"')
                modelName.erase(modelName.begin());
            while (!modelName.empty() && modelName.back() == L'"')
                modelName.pop_back();
        }
        modelName = TrimWide(modelName);

        if (modelName.size() >= 3)
            return modelName;
    }

    // 策略2：回退 - 在任意节中找 "ModelName" =
    for (const std::wstring& line : lines)
    {
        std::wstring trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() != L'"')
            continue;

        size_t eq = trimmed.find(L'=');
        if (eq == std::wstring::npos || eq == 0)
            continue;

        std::wstring right = trimmed.substr(eq + 1);
        right = TrimWide(right);
        {
            while (!right.empty() && right.front() == L'"')
                right.erase(right.begin());
            while (!right.empty() && right.back() == L'"')
                right.pop_back();
        }

        if (right.size() < 3)
            continue;

        if (containsIgnoreCase(right, L"disk"))
            continue;

        bool allDigits = true;
        for (wchar_t ch : right)
        {
            if (!iswdigit(ch))
            {
                allDigits = false;
                break;
            }
        }

        if (allDigits)
            continue;

        return right;
    }

    return std::wstring();
}

// ===========================================================================
// 打印机查询
// ===========================================================================

// 回读驱动库，判断某驱动是否真的已注册。
// 关键：rundll32/printui 的退出码恒为 0，不能作为安装成功的依据，
// 必须用系统枚举结果判真，否则会出现“日志说成功、实际没装上”的假成功。
bool IsPrinterDriverInstalled(const std::wstring& driverName)
{
    if (driverName.empty())
        return false;

    static const wchar_t* kEnvironments[] = { L"Windows x64", L"Windows NT x86" };

    for (const wchar_t* environment : kEnvironments)
    {
        // winspool.h 把环境名声明为 LPWSTR（非 const），这里显式转换
        LPWSTR environmentName = const_cast<LPWSTR>(environment);

        DWORD needed = 0;
        DWORD returned = 0;

        // 首次调用只取所需字节数（返回 FALSE + ERROR_INSUFFICIENT_BUFFER 属预期）
        ::EnumPrinterDriversW(nullptr, environmentName, 3, nullptr, 0, &needed, &returned);
        if (needed == 0)
            continue;

        std::vector<unsigned char> buffer(needed);
        if (!::EnumPrinterDriversW(nullptr, environmentName, 3, buffer.data(), needed,
                                   &needed, &returned))
        {
            continue;
        }

        const DRIVER_INFO_3W* info = (const DRIVER_INFO_3W*)buffer.data();
        for (DWORD i = 0; i < returned; ++i)
        {
            if (info[i].pName != nullptr &&
                _wcsicmp(info[i].pName, driverName.c_str()) == 0)
            {
                return true;
            }
        }
    }

    return false;
}

// 对应 PrinterExists(printerName)：SELECT Name FROM Win32_Printer WHERE Name = '...'
bool PrinterExists(const std::string& printerName)
{
    try
    {
        std::wstring escaped = Utf8ToWide(printerName);

        std::wstring quoted;
        for (wchar_t ch : escaped)
        {
            if (ch == L'\\')
                quoted += L"\\\\";
            else if (ch == L'\'')
                quoted += L"''";
            else
                quoted.push_back(ch);
        }

        std::wstring wql = L"SELECT Name FROM Win32_Printer WHERE Name = '" + quoted + L"'";

        WmiQuery query;
        if (!query.IsValid())
            return false;

        bool found = false;
        query.ForEach(wql, [&found](IWbemClassObject*) {
            found = true;
            return false;
        });

        return found;
    }
    catch (...)
    {
        return false;
    }
}

// 对应 PrintuiPrinterExists：SELECT * FROM Win32_Printer WHERE Name = '...'（仅转义单引号）
bool PrintuiPrinterExists(const std::string& printerName)
{
    try
    {
        std::wstring escaped = Utf8ToWide(printerName);

        std::wstring quoted;
        for (wchar_t ch : escaped)
        {
            if (ch == L'\'')
                quoted += L"''";
            else
                quoted.push_back(ch);
        }

        std::wstring wql = L"SELECT * FROM Win32_Printer WHERE Name = '" + quoted + L"'";

        WmiQuery query;
        if (!query.IsValid())
            return false;

        bool found = false;
        query.ForEach(wql, [&found](IWbemClassObject*) {
            found = true;
            return false;
        });

        return found;
    }
    catch (...)
    {
        return false;
    }
}

// ===========================================================================
// 端口创建
// ===========================================================================

// 对应 CreatePortViaWmi：WMI Win32_TCPIPPrinterPort + PutType.CreateOnly
bool CreatePortViaWmi(const std::string& host, int port, const std::string& portName,
                      const InstallProgressFn& progress)
{
    WmiEnsureComInitialized();

    IWbemLocator* locator = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWbemLocator, (LPVOID*)&locator);
    if (FAILED(hr) || locator == nullptr)
    {
        ReportProgress(progress, "WMI 端口创建失败: 无法创建 WMI 定位器");
        return false;
    }

    IWbemServices* services = nullptr;
    BSTR ns = ::SysAllocString(L"ROOT\\CIMV2");
    hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    ::SysFreeString(ns);
    locator->Release();

    if (FAILED(hr) || services == nullptr)
    {
        ReportProgress(progress, "WMI 端口创建失败: 无法连接 WMI 命名空间");
        return false;
    }

    ::CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                        nullptr, EOAC_NONE);

    IWbemClassObject* portClass = nullptr;
    hr = services->GetObject(L"Win32_TCPIPPrinterPort", 0, nullptr, &portClass, nullptr);

    if (FAILED(hr) || portClass == nullptr)
    {
        ReportProgress(progress, "WMI 端口创建失败: 无法获取端口类定义");
        services->Release();
        return false;
    }

    IWbemClassObject* instance = nullptr;
    hr = portClass->SpawnInstance(0, &instance);
    portClass->Release();

    if (FAILED(hr) || instance == nullptr)
    {
        ReportProgress(progress, "WMI 端口创建失败: 无法创建端口实例");
        services->Release();
        return false;
    }

    auto putString = [&instance](const wchar_t* name, const std::wstring& value) {
        VARIANT vt;
        ::VariantInit(&vt);
        vt.vt = VT_BSTR;
        vt.bstrVal = ::SysAllocString(value.c_str());
        instance->Put(name, 0, &vt, CIM_STRING);
        ::VariantClear(&vt);
    };

    auto putInt = [&instance](const wchar_t* name, long value) {
        VARIANT vt;
        ::VariantInit(&vt);
        vt.vt = VT_I4;
        vt.lVal = value;
        instance->Put(name, 0, &vt, CIM_UINT32);
        ::VariantClear(&vt);
    };

    auto putBool = [&instance](const wchar_t* name, bool value) {
        VARIANT vt;
        ::VariantInit(&vt);
        vt.vt = VT_BOOL;
        vt.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        instance->Put(name, 0, &vt, CIM_BOOLEAN);
        ::VariantClear(&vt);
    };

    putString(L"Name", Utf8ToWide(portName));
    putInt(L"Protocol", 1);     // 1 = RAW
    putString(L"HostAddress", Utf8ToWide(host));
    putInt(L"PortNumber", port);
    putBool(L"SNMPEnabled", false);

    hr = services->PutInstance(instance, WBEM_FLAG_CREATE_ONLY, nullptr, nullptr);

    instance->Release();
    services->Release();

    if (SUCCEEDED(hr))
    {
        ReportProgress(progress, "WMI 端口创建成功");
        return true;
    }

    // 端口可能已存在
    if (hr == kManagementAlreadyExists)
    {
        ReportProgress(progress, "端口已存在，将直接使用");
        return true;
    }

    std::string message;
    {
        _com_error error(hr);
        message = WideToUtf8(error.ErrorMessage());
    }
    ReportProgress(progress, "WMI 端口创建失败: " + message);
    return false;
}

// 对应 CreatePortViaPrintUI（实际用 PowerShell Add-PrinterPort）
bool CreatePortViaPrintUI(const std::string& host, int port, const std::string& portName,
                          const InstallProgressFn& progress)
{
    // 单引号内部转义为 ''
    std::wstring quotedPort = Utf8ToWide(portName);
    {
        std::wstring temp;
        for (wchar_t ch : quotedPort)
        {
            if (ch == L'\'')
                temp += L"''";
            else
                temp.push_back(ch);
        }
        quotedPort = temp;
    }

    std::wstring quotedHost = Utf8ToWide(host);
    {
        std::wstring temp;
        for (wchar_t ch : quotedHost)
        {
            if (ch == L'\'')
                temp += L"''";
            else
                temp.push_back(ch);
        }
        quotedHost = temp;
    }

    // 对应 string.Format("Add-PrinterPort -Name '{0}' -PrinterHostAddress '{1}' -PortNumber {2}")
    std::wstring command = L"Add-PrinterPort -Name '" + quotedPort +
                           L"' -PrinterHostAddress '" + quotedHost +
                           L"' -PortNumber " + std::to_wstring(port);

    std::wstring arguments = L"-NoProfile -Command \"" + command + L"\"";

    ProcessResult process;
    if (!RunProcess(L"powershell.exe", arguments, 15000, process) || !process.started)
    {
        ReportProgress(progress, "printui 创建端口失败: 无法启动 PowerShell");
        return false;
    }

    if (process.exitCode == 0)
    {
        ReportProgress(progress, "PowerShell 端口创建成功");
        return true;
    }

    ReportProgress(progress, "PowerShell 端口创建输出: " + TrimAscii(process.output + process.error));
    return false;
}

// 对应 CreateTcpIpPort
bool CreateTcpIpPort(const std::string& host, int port, const std::string& portName,
                     const InstallProgressFn& progress)
{
    try
    {
        // 方法1：使用 WMI 创建 TCP/IP 打印机端口
        return CreatePortViaWmi(host, port, portName, progress);
    }
    catch (const std::exception& ex)
    {
        ReportProgress(progress, std::string("WMI 创建端口失败: ") + ex.what());

        // 方法2：使用 PowerShell Add-PrinterPort
        return CreatePortViaPrintUI(host, port, portName, progress);
    }
}

// ===========================================================================
// 端口双向配置
// ===========================================================================

// 对应 ConfigurePortBidirectional
bool ConfigurePortBidirectional(const std::string& portName, const InstallProgressFn& progress)
{
    std::wstring regPath = L"SYSTEM\\CurrentControlSet\\Control\\Print\\Monitors\\"
                           L"Standard TCP/IP Port\\Ports\\" + Utf8ToWide(portName);

    HKEY key = nullptr;
    LONG status = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, KEY_SET_VALUE, &key);
    if (status != ERROR_SUCCESS || key == nullptr)
    {
        ReportProgress(progress, "端口注册表项不存在: " + WideToUtf8(regPath));
        return false;
    }

    auto setDword = [&key](const wchar_t* name, DWORD value) {
        ::RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    };

    setDword(L"EnableBidi", 1);
    setDword(L"SNMP Enabled", 0);
    setDword(L"Protocol", 1);

    ::RegCloseKey(key);

    ReportProgress(progress, "✅ EnableBidi=1 SNMP=Off");
    return true;
}

// ===========================================================================
// 驱动安装
// ===========================================================================

// 对应 InstallDriverViaPrintUI
bool InstallDriverViaPrintUI(const std::string& infPath, const std::string& driverName,
                             const InstallProgressFn& progress)
{
    ReportProgress(progress, "正在使用 printui.dll 安装驱动...");

    // 对应 string.Format("printui.dll,PrintUIEntry /ia /m \"{0}\" /f \"{1}\" /h \"x64\" /q")
    std::wstring arguments = L"printui.dll,PrintUIEntry /ia /m \"" + Utf8ToWide(driverName) +
                             L"\" /f \"" + Utf8ToWide(infPath) + L"\" /h \"x64\" /q";

    ProcessResult process;
    if (!RunProcess(L"rundll32.exe", arguments, 120000, process) || !process.started)
    {
        ReportProgress(progress, "printui.dll 安装异常: 无法启动 rundll32.exe");
        return false;
    }

    // rundll32 的退出码恒为 0，不能作为成功依据：
    // 必须回读驱动库确认驱动真的注册成功，否则会误报成功，
    // 并因 || 短路让后面的 pnputil / API 回退分支永远不执行。
    if (IsPrinterDriverInstalled(Utf8ToWide(driverName)))
    {
        ReportProgress(progress, "printui.dll 驱动安装成功");
        return true;
    }

    if (process.exitCode != 0)
    {
        ReportProgress(progress, "printui.dll 输出: " + TrimAscii(process.output + process.error));
    }
    else
    {
        ReportProgress(progress, "printui.dll 未真正注册驱动，改用其它方式");
    }

    return false;
}

// 对应 InstallDriverViaPnpUtil
bool InstallDriverViaPnpUtil(const std::string& infPath, const std::string& driverName,
                             const InstallProgressFn& progress)
{
    ReportProgress(progress, "正在使用 pnputil 安装驱动...");

    std::wstring arguments = L"/add-driver \"" + Utf8ToWide(infPath) + L"\" /install";

    ProcessResult process;
    if (!RunProcess(L"pnputil.exe", arguments, 120000, process) || !process.started)
    {
        ReportProgress(progress, "pnputil 安装失败: 无法启动 pnputil.exe");
        return false;
    }

    std::string combined = process.output + process.error;

    bool success = false;
    {
        std::string lower = combined;
        for (char& ch : lower)
        {
            if (ch >= 'A' && ch <= 'Z')
                ch = (char)(ch - 'A' + 'a');
        }

        if (lower.find("driver package added") != std::string::npos ||
            lower.find("successfully") != std::string::npos ||
            process.exitCode == 0)
        {
            success = true;
        }
    }

    if (!success)
    {
        ReportProgress(progress, "pnputil 输出: " + TrimAscii(combined));
        return false;
    }

    // 同样以驱动库回读为准，避免把「驱动包已存在/未真正注册」当成成功
    if (!IsPrinterDriverInstalled(Utf8ToWide(driverName)))
    {
        ReportProgress(progress, "pnputil 已处理驱动包，但驱动未出现在驱动库中");
        return false;
    }

    ReportProgress(progress, "pnputil 驱动安装成功");
    return true;
}

// 对应 InstallDriverViaApi
bool InstallDriverViaApi(const std::string& driverDir, const std::string& driverName,
                         const std::string& infPath, const InstallProgressFn& progress)
{
    UNREFERENCED_PARAMETER(infPath);

    ReportProgress(progress, "正在使用 API 安装驱动...");

    std::vector<std::wstring> driverFiles;
    EnumerateFilesRecursive(Utf8ToWide(driverDir), L"*.*", driverFiles);

    auto endsWith = [](const std::wstring& text, const wchar_t* suffix) {
        size_t length = wcslen(suffix);
        if (text.size() < length)
            return false;
        return _wcsicmp(text.c_str() + (text.size() - length), suffix) == 0;
    };

    std::wstring driverFile;
    std::wstring configFile;
    std::wstring dataFile;
    std::wstring helpFile;

    for (const std::wstring& file : driverFiles)
    {
        if (driverFile.empty() && endsWith(file, L".dll") && !endsWith(file, L"ui.dll"))
            driverFile = file;
    }

    for (const std::wstring& file : driverFiles)
    {
        if (configFile.empty() && endsWith(file, L".dll"))
            configFile = file;
    }

    for (const std::wstring& file : driverFiles)
    {
        if (dataFile.empty() && (endsWith(file, L".ppd") || endsWith(file, L".gpd")))
            dataFile = file;
    }

    for (const std::wstring& file : driverFiles)
    {
        if (helpFile.empty() && (endsWith(file, L".hlp") || endsWith(file, L".chm")))
            helpFile = file;
    }

    // 对应 string.Join("\0", driverFiles) + "\0"
    std::wstring dependentFiles;
    for (const std::wstring& file : driverFiles)
    {
        dependentFiles += file;
        dependentFiles.push_back(L'\0');
    }
    dependentFiles.push_back(L'\0');

    DRIVER_INFO_3W driverInfo;
    memset(&driverInfo, 0, sizeof(driverInfo));
    driverInfo.cVersion = 3;
    driverInfo.pEnvironment = const_cast<LPWSTR>(L"Windows x64");
    driverInfo.pDriverPath = const_cast<LPWSTR>(driverFile.c_str());
    driverInfo.pDataFile = const_cast<LPWSTR>(dataFile.c_str());
    driverInfo.pConfigFile = const_cast<LPWSTR>(configFile.c_str());
    driverInfo.pHelpFile = const_cast<LPWSTR>(helpFile.c_str());
    driverInfo.pDependentFiles = const_cast<LPWSTR>(dependentFiles.c_str());
    driverInfo.pMonitorName = nullptr;
    driverInfo.pDefaultDataType = const_cast<LPWSTR>(L"RAW");

    std::wstring wideDriverName = Utf8ToWide(driverName);
    driverInfo.pName = const_cast<LPWSTR>(wideDriverName.c_str());

    BOOL success = ::AddPrinterDriverExW(nullptr, 3, (PBYTE)&driverInfo,
                                         APD_COPY_ALL_FILES | APD_COPY_FROM_DIRECTORY);

    if (!success)
    {
        DWORD error = ::GetLastError();
        char message[128] = { 0 };
        _snprintf_s(message, sizeof(message), _TRUNCATE,
                    "API 安装驱动失败，错误代码: %lu", error);
        ReportProgress(progress, message);
        return false;
    }

    // 以驱动库回读为准：API 报成功但库里查不到时仍按失败处理
    if (!IsPrinterDriverInstalled(wideDriverName))
    {
        char message[192] = { 0 };
        _snprintf_s(message, sizeof(message), _TRUNCATE,
                    "API 报告成功但驱动库中查不到驱动: %s", driverName.c_str());
        ReportProgress(progress, message);
        return false;
    }

    return true;
}

// ===========================================================================
// 打印机创建
// ===========================================================================

// 对应 CreatePrinterViaPowerShell
bool CreatePrinterViaPowerShell(const std::string& printerName, const std::string& portName,
                                const std::string& driverName, const InstallProgressFn& progress)
{
    auto quote = [](const std::string& text) {
        std::wstring result;
        for (wchar_t ch : Utf8ToWide(text))
        {
            if (ch == L'\'')
                result += L"''";
            else
                result.push_back(ch);
        }
        return result;
    };

    // 对应 string.Format("Add-Printer -Name '{0}' -DriverName '{1}' -PortName '{2}'")
    std::wstring command = L"Add-Printer -Name '" + quote(printerName) +
                           L"' -DriverName '" + quote(driverName) +
                           L"' -PortName '" + quote(portName) + L"'";

    std::wstring arguments = L"-NoProfile -Command \"" + command + L"\"";

    ProcessResult process;
    if (!RunProcess(L"powershell.exe", arguments, 30000, process) || !process.started)
    {
        ReportProgress(progress, "PowerShell 创建打印机失败: 无法启动 PowerShell");
        return false;
    }

    // PowerShell 退出码同样不能单独作为依据：必须回读打印机列表确认
    if (PrinterExists(printerName) || PrinterExistsByApi(printerName))
    {
        ReportProgress(progress, "PowerShell 打印机创建成功");
        return true;
    }

    if (process.exitCode != 0)
    {
        ReportProgress(progress, "PowerShell 输出: " + TrimAscii(process.output + process.error));
    }
    else
    {
        ReportProgress(progress, "PowerShell 未真正创建打印机");
    }

    return false;
}

// 对应 CreatePrinterViaPrintUI
bool CreatePrinterViaPrintUI(const std::string& printerName, const std::string& portName,
                             const std::string& driverName, const std::string& infPath,
                             const InstallProgressFn& progress)
{
    ReportProgress(progress, "正在使用 printui.dll 创建打印机...");

    // 对应 string.Format("printui.dll,PrintUIEntry /if /b \"{0}\" /f \"{1}\" /r \"{2}\" /m \"{3}\" /q")
    std::wstring arguments = L"printui.dll,PrintUIEntry /if /b \"" + Utf8ToWide(printerName) +
                             L"\" /f \"" + Utf8ToWide(infPath) +
                             L"\" /r \"" + Utf8ToWide(portName) +
                             L"\" /m \"" + Utf8ToWide(driverName) + L"\" /q";

    ProcessResult process;
    if (!RunProcess(L"rundll32.exe", arguments, 120000, process) || !process.started)
    {
        ReportProgress(progress, "printui.dll 创建打印机异常: 无法启动 rundll32.exe");
        return false;
    }

    // 无论退出码如何，一律以“打印机是否真的存在”为准
    if (PrinterExists(printerName) || PrinterExistsByApi(printerName) ||
        PrintuiPrinterExists(printerName))
    {
        ReportProgress(progress, "printui.dll 打印机创建成功");
        return true;
    }

    ReportProgress(progress, "printui.dll 创建输出: " + TrimAscii(process.output + process.error));
    return false;
}

// 对应 CreatePrinterViaApi
bool CreatePrinterViaApi(const std::string& printerName, const std::string& portName,
                         const std::string& driverName, const InstallProgressFn& progress)
{
    std::wstring widePrinterName = Utf8ToWide(printerName);
    std::wstring widePortName = Utf8ToWide(portName);
    std::wstring wideDriverName = Utf8ToWide(driverName);
    std::wstring comment = Utf8ToWide("网络打印客户端 - 连接到打印服务器");

    PRINTER_INFO_2W printerInfo;
    memset(&printerInfo, 0, sizeof(printerInfo));
    printerInfo.pPrinterName = const_cast<LPWSTR>(widePrinterName.c_str());
    printerInfo.pPortName = const_cast<LPWSTR>(widePortName.c_str());
    printerInfo.pDriverName = const_cast<LPWSTR>(wideDriverName.c_str());
    printerInfo.pPrintProcessor = const_cast<LPWSTR>(L"WinPrint");
    printerInfo.pDatatype = const_cast<LPWSTR>(L"RAW");
    printerInfo.pShareName = const_cast<LPWSTR>(L"");
    printerInfo.pComment = const_cast<LPWSTR>(comment.c_str());
    printerInfo.pLocation = const_cast<LPWSTR>(L"");
    printerInfo.pSepFile = const_cast<LPWSTR>(L"");
    printerInfo.pParameters = const_cast<LPWSTR>(L"");
    printerInfo.Attributes = 0;
    printerInfo.Priority = 1;
    printerInfo.DefaultPriority = 1;
    printerInfo.StartTime = 0;
    printerInfo.UntilTime = 0;
    printerInfo.Status = 0;
    printerInfo.cJobs = 0;
    printerInfo.AveragePPM = 0;

    // 注意：winspool.h 中 AddPrinterW 的返回值类型是 HANDLE（.NET 的 DllImport 声明为 bool，
    // 非空句柄即 true）；对应 C# 不关闭该句柄的写法，这里顺手关闭，避免句柄泄漏。
    HANDLE handle = ::AddPrinterW(nullptr, 2, (LPBYTE)&printerInfo);
    BOOL success = (handle != nullptr);

    if (handle != nullptr)
        ::ClosePrinter(handle);

    if (!success)
    {
        DWORD error = ::GetLastError();
        char message[128] = { 0 };
        _snprintf_s(message, sizeof(message), _TRUNCATE,
                    "API 创建打印机失败，错误代码: %lu", error);
        ReportProgress(progress, message);
        return false;
    }

    if (!PrinterExists(printerName) && !PrinterExistsByApi(printerName))
    {
        ReportProgress(progress, "API 报告成功但打印机列表中查不到该打印机");
        return false;
    }

    return true;
}

// 对应 CreatePrinter
bool CreatePrinter(const std::string& printerName, const std::string& portName,
                   const std::string& driverName, const InstallProgressFn& progress)
{
    try
    {
        // 方法1：使用 PowerShell Add-Printer
        return CreatePrinterViaPowerShell(printerName, portName, driverName, progress);
    }
    catch (...)
    {
        // 方法2：使用 Win32 API
        return CreatePrinterViaApi(printerName, portName, driverName, progress);
    }
}

// 对应 InstallDriverAndPrinter（printui /if 一步法）
bool InstallDriverAndPrinter(const std::string& infPath, const std::string& driverName,
                             const std::string& printerName, const std::string& portName,
                             const InstallProgressFn& progress)
{
    try
    {
        ReportProgress(progress, "正在一步安装驱动和打印机...");

        std::wstring arguments = L"printui.dll,PrintUIEntry /if /b \"" + Utf8ToWide(printerName) +
                                 L"\" /f \"" + Utf8ToWide(infPath) +
                                 L"\" /r \"" + Utf8ToWide(portName) +
                                 L"\" /m \"" + Utf8ToWide(driverName) + L"\" /q";

        ProcessResult process;
        if (!RunProcess(L"rundll32.exe", arguments, 120000, process) || !process.started)
        {
            ReportProgress(progress, "一步安装异常: 无法启动 rundll32.exe");
            return false;
        }

        char message[128] = { 0 };
        _snprintf_s(message, sizeof(message), _TRUNCATE,
                    "printui /if 退出码: %lu", process.exitCode);
        ReportProgress(progress, message);

        if (process.exitCode != 0)
            ReportProgress(progress, "输出: " + TrimAscii(process.output + process.error));

        // 一步法同样以实际结果为准（rundll32 的退出码不可信）
        return PrinterExists(printerName) || PrinterExistsByApi(printerName) ||
               IsPrinterDriverInstalled(Utf8ToWide(driverName));
    }
    catch (const std::exception& ex)
    {
        ReportProgress(progress, std::string("一步安装异常: ") + ex.what());
        return false;
    }
}

// 对应 RestartSpooler
void RestartSpooler()
{
    try
    {
        ProcessResult process;
        RunProcess(L"net", L"stop spooler", 10000, process);

        ::Sleep(1000);

        RunProcess(L"net", L"start spooler", 10000, process);
    }
    catch (...)
    {
    }
}

} // namespace

InstallResult InstallPrinter(const std::string& driverDirectory, const std::string& serverHost,
                             int serverPort, const std::string& printerName,
                             const std::string& driverName, const InstallProgressFn& progress)
{
    InstallResult result;

    try
    {
        std::string effectiveDriverName = driverName;

        // 清单：新服务端会在驱动包内附带，用于唯一定位 INF / 精确驱动名 / 完整性说明
        DriverManifest manifest;
        DriverDownloader::LoadManifest(driverDirectory, manifest);
        if (manifest.present)
        {
            result.steps.push_back("📦 驱动包清单: 来源=" +
                                   (manifest.source.empty() ? std::string("未知") : manifest.source) +
                                   ", 架构=" +
                                   (manifest.architecture.empty() ? std::string("未知") : manifest.architecture));
            if (!manifest.complete && !manifest.missing.empty())
            {
                result.steps.push_back("⚠ 服务端自检提示: 驱动包可能缺少 " +
                                       std::to_string(manifest.missing.size()) + " 个文件");
            }
        }

        // 步骤1：查找驱动 INF 文件（清单指定 > 按驱动名匹配 > 兜底）
        ReportProgress(progress, "正在查找驱动 INF 文件...");
        std::wstring infPath = FindInfFile(Utf8ToWide(driverDirectory),
                                           Utf8ToWide(manifest.inf), driverName);
        if (infPath.empty())
        {
            result.message = "未在驱动包中找到 INF 文件";
            result.steps.push_back("❌ 查找 INF 文件失败");
            return result;
        }

        result.steps.push_back("✅ 找到 INF 文件: " + WideToUtf8(GetFileName(infPath)));

        // 驱动名：清单给出的精确名优先，其次清单里的 INF 型号名，
        // 再次 INF 解析结果，最后用接口传入值
        if (manifest.present && !manifest.driverName.empty())
        {
            effectiveDriverName = manifest.driverName;
            ReportProgress(progress, "清单中的驱动名: " + effectiveDriverName);
        }
        else if (manifest.present && !manifest.infModels.empty())
        {
            effectiveDriverName = manifest.infModels[0];
            ReportProgress(progress, "清单 INF 型号名: " + effectiveDriverName);
        }
        else
        {
            std::wstring realDriverName = ReadDriverNameFromInf(infPath);
            if (!realDriverName.empty())
            {
                effectiveDriverName = WideToUtf8(realDriverName);
                ReportProgress(progress, "INF中的驱动名: " + effectiveDriverName);
            }
        }

        // 步骤2：创建 TCP/IP 打印端口
        std::string portName = BuildPortName(serverHost, serverPort);

        ReportProgress(progress, "正在创建打印端口...");
        if (!CreateTcpIpPort(serverHost, serverPort, portName, progress))
        {
            result.message = "创建打印端口失败";
            result.steps.push_back("❌ 创建端口失败");
            return result;
        }

        result.steps.push_back("✅ 打印端口已创建: " + portName);
        result.portName = portName;

        // 步骤2.5：写入端口双向参数。
        // 注意：服务端目前只做 RAW 直通、不应答 PJL 状态查询，
        // 因此这里写的 EnableBidi 尚不能让“双向通信”真正生效，
        // 日志按实际情况描述，避免误报能力。
        ReportProgress(progress, "正在写入端口双向参数...");
        if (ConfigurePortBidirectional(portName, progress))
            result.steps.push_back(
                "ℹ 端口双向参数已写入(EnableBidi=1/SNMP=Off)，需服务端支持 PJL 应答才生效");
        else
            result.steps.push_back("⚠ 端口双向参数写入失败（不影响基本打印）");

        // 步骤3：安装驱动 + 创建打印机
        ReportProgress(progress, "正在安装驱动并创建打印机...");
        bool installed = false;

        bool driverOk = InstallDriverViaPrintUI(WideToUtf8(infPath), effectiveDriverName, progress)
                        || InstallDriverViaPnpUtil(WideToUtf8(infPath), effectiveDriverName, progress)
                        || InstallDriverViaApi(driverDirectory, effectiveDriverName,
                                               WideToUtf8(infPath), progress);

        if (driverOk)
        {
            // PowerShell 创建打印机最可靠
            installed = CreatePrinterViaPowerShell(printerName, portName, effectiveDriverName, progress)
                        || CreatePrinterViaPrintUI(printerName, portName, effectiveDriverName,
                                                   WideToUtf8(infPath), progress)
                        || CreatePrinter(printerName, portName, effectiveDriverName, progress);
        }

        if (!installed)
        {
            // 最后兜底：一步法
            ReportProgress(progress, "尝试一步安装...");
            installed = InstallDriverAndPrinter(WideToUtf8(infPath), effectiveDriverName,
                                                printerName, portName, progress);
        }

        RestartSpooler();
        ::Sleep(2000);

        if (!PrinterExists(printerName))
        {
            // 区分两种失败原因，避免一句“驱动不兼容”掩盖真实问题
            if (!IsPrinterDriverInstalled(Utf8ToWide(effectiveDriverName)))
            {
                result.message = "驱动安装失败：驱动未进入系统驱动库，请确认以管理员身份运行后重试";
                result.steps.push_back("❌ 驱动未安装到系统");
            }
            else
            {
                result.message = "打印机创建失败，请检查驱动是否兼容当前Windows版本";
                result.steps.push_back("❌ 打印机未在系统中出现");
            }

            return result;
        }

        result.steps.push_back("✅ 驱动已安装到系统");
        result.steps.push_back("✅ 打印机已创建: " + printerName);
        result.printerName = printerName;
        result.driverName = effectiveDriverName;

        result.success = true;
        result.message = "打印机安装成功！";
        result.steps.push_back("🎉 全部完成！打印机 " + printerName + " 已就绪");
    }
    catch (const std::exception& ex)
    {
        result.success = false;
        result.message = std::string("安装过程中发生错误: ") + ex.what();
        result.steps.push_back(std::string("❌ 错误: ") + ex.what());
    }
    catch (...)
    {
        result.success = false;
        result.message = "安装过程中发生错误: 未知错误";
        result.steps.push_back("❌ 错误: 未知错误");
    }

    return result;
}

// ===========================================================================
// 命令行自检 / 清理辅助（安装路径不使用）
// ===========================================================================

std::string BuildPortName(const std::string& serverHost, int serverPort)
{
    char portBuffer[512] = { 0 };
    _snprintf_s(portBuffer, sizeof(portBuffer), _TRUNCATE, "PS_%s_%d", serverHost.c_str(),
                serverPort);
    return portBuffer;
}

bool RunCommand(const std::wstring& fileName, const std::wstring& arguments, DWORD timeoutMs,
                CommandResult& result)
{
    ProcessResult process;
    bool started = RunProcess(fileName, arguments, timeoutMs, process);

    result.started = started && process.started;
    result.timedOut = process.timedOut;
    result.exitCode = process.exitCode;
    result.output = process.output;
    result.error = process.error;

    return result.started;
}

bool DeleteTcpIpPrinterPortViaWmi(const std::string& portName, std::string& error)
{
    try
    {
        WmiEnsureComInitialized();

        // 1) 先查出实例的对象路径（复用已验证的 WmiQuery）
        std::wstring escaped = Utf8ToWide(portName);
        std::wstring quoted;
        for (wchar_t ch : escaped)
        {
            if (ch == L'\'')
                quoted += L"''";
            else
                quoted.push_back(ch);
        }

        std::wstring wql = L"SELECT * FROM Win32_TCPIPPrinterPort WHERE Name = '" + quoted + L"'";

        std::wstring objectPath;
        {
            WmiQuery query;
            if (!query.IsValid())
            {
                error = "无法连接 WMI 命名空间";
                return false;
            }

            bool found = false;
            query.ForEach(wql, [&](IWbemClassObject* row) {
                found = WmiQuery::GetString(row, L"__PATH", objectPath);
                return false;
            });

            if (!found || objectPath.empty())
            {
                error = "未找到该端口实例";
                return false;
            }
        }

        // 2) 删除实例
        IWbemLocator* locator = nullptr;
        HRESULT hr = ::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_IWbemLocator, (LPVOID*)&locator);
        if (FAILED(hr) || locator == nullptr)
        {
            error = "无法创建 WMI 定位器";
            return false;
        }

        IWbemServices* services = nullptr;
        BSTR ns = ::SysAllocString(L"ROOT\\CIMV2");
        hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
        ::SysFreeString(ns);
        locator->Release();

        if (FAILED(hr) || services == nullptr)
        {
            error = "无法连接 WMI 命名空间";
            return false;
        }

        ::CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                            EOAC_NONE);

        BSTR path = ::SysAllocString(objectPath.c_str());
        hr = services->DeleteInstance(path, 0, nullptr, nullptr);
        ::SysFreeString(path);
        services->Release();

        if (SUCCEEDED(hr))
            return true;

        std::string message;
        {
            _com_error comError(hr);
            message = WideToUtf8(comError.ErrorMessage());
        }
        error = message;
        return false;
    }
    catch (const std::exception& ex)
    {
        error = ex.what();
        return false;
    }
    catch (...)
    {
        error = "未知错误";
        return false;
    }
}

bool RemovePortRegistryKey(const std::string& portName, bool& removed)
{
    removed = false;

    std::wstring subKey = L"SYSTEM\\CurrentControlSet\\Control\\Print\\Monitors\\"
                          L"Standard TCP/IP Port\\Ports\\" + Utf8ToWide(portName);

    HKEY key = nullptr;
    LONG status = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey.c_str(), 0, KEY_READ, &key);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
        return true; // 已不存在

    if (status != ERROR_SUCCESS)
        return false;

    ::RegCloseKey(key);

    status = ::RegDeleteTreeW(HKEY_LOCAL_MACHINE, subKey.c_str());
    removed = (status == ERROR_SUCCESS);
    return removed || status == ERROR_FILE_NOT_FOUND;
}

bool PrinterExistsByApi(const std::string& printerName)
{
    if (printerName.empty())
        return false;

    HANDLE handle = nullptr;
    std::wstring target = Utf8ToWide(printerName);
    if (::OpenPrinterW(const_cast<LPWSTR>(target.c_str()), &handle, nullptr) && handle != nullptr)
    {
        ::ClosePrinter(handle);
        return true;
    }

    // 兜底：完整枚举本地打印机
    DWORD needed = 0;
    DWORD returned = 0;
    ::EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2, nullptr, 0, &needed,
                    &returned);
    if (needed == 0)
        return false;

    std::vector<BYTE> buffer(needed);
    if (!::EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2, buffer.data(),
                         needed, &needed, &returned))
        return false;

    PRINTER_INFO_2W* items = (PRINTER_INFO_2W*)buffer.data();

    for (DWORD i = 0; i < returned; ++i)
    {
        if (items[i].pPrinterName != nullptr && _wcsicmp(items[i].pPrinterName, target.c_str()) == 0)
            return true;
    }

    return false;
}

bool GetPrinterPortNameByApi(const std::string& printerName, std::string& portName)
{
    portName.clear();
    if (printerName.empty())
        return false;

    DWORD needed = 0;
    DWORD returned = 0;
    ::EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2, nullptr, 0, &needed,
                    &returned);
    if (needed == 0)
        return false;

    std::vector<BYTE> buffer(needed);
    if (!::EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2, buffer.data(),
                         needed, &needed, &returned))
        return false;

    PRINTER_INFO_2W* items = (PRINTER_INFO_2W*)buffer.data();
    std::wstring target = Utf8ToWide(printerName);

    for (DWORD i = 0; i < returned; ++i)
    {
        if (items[i].pPrinterName != nullptr && _wcsicmp(items[i].pPrinterName, target.c_str()) == 0)
        {
            if (items[i].pPortName != nullptr)
                portName = WideToUtf8(items[i].pPortName);

            return true;
        }
    }

    return false;
}

bool PortExistsByApi(const std::string& portName)
{
    if (portName.empty())
        return false;

    DWORD needed = 0;
    DWORD returned = 0;
    ::EnumPortsW(nullptr, 2, nullptr, 0, &needed, &returned);
    if (needed == 0)
        return false;

    std::vector<BYTE> buffer(needed);
    if (!::EnumPortsW(nullptr, 2, buffer.data(), needed, &needed, &returned))
        return false;

    PORT_INFO_2W* items = (PORT_INFO_2W*)buffer.data();
    std::wstring target = Utf8ToWide(portName);

    for (DWORD i = 0; i < returned; ++i)
    {
        if (items[i].pPortName != nullptr && _wcsicmp(items[i].pPortName, target.c_str()) == 0)
            return true;
    }

    return false;
}

// ===========================================================================
// IPP 免驱安装
// ===========================================================================

// 找出系统自带的 IPP 类驱动 INF。
// 不同 Windows 版本文件名不同（本机是 prnms012.inf），所以不能写死，
// 而是扫 %SystemRoot%\INF\prnms*.inf，找内容里带 MS_IPP 硬件 ID 的那个。
// INF 通常是 UTF-16，因此同时按 ASCII 与 UTF-16 两种形式匹配。
std::wstring FindIppClassDriverInf()
{
    wchar_t windowsDirectory[MAX_PATH] = { 0 };
    if (::GetWindowsDirectoryW(windowsDirectory, MAX_PATH) == 0)
        return std::wstring();

    std::wstring infDirectory = PathCombine(std::wstring(windowsDirectory), L"INF");
    std::wstring pattern = PathCombine(infDirectory, L"prnms*.inf");

    // 需要匹配的两种编码形式
    const std::string asciiNeedle = "MS_IPP";
    std::string wideNeedle;
    for (char ch : asciiNeedle)
    {
        wideNeedle.push_back(ch);
        wideNeedle.push_back('\0');
    }

    WIN32_FIND_DATAW entry;
    memset(&entry, 0, sizeof(entry));

    HANDLE find = ::FindFirstFileW(pattern.c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE)
        return std::wstring();

    std::wstring found;

    do
    {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            continue;

        std::wstring path = PathCombine(infDirectory, entry.cFileName);

        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            continue;

        std::string content;
        content.reserve(256 * 1024);

        char buffer[8192];
        DWORD read = 0;
        bool matched = false;

        while (::ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0)
        {
            content.append(buffer, read);
            if (content.find(asciiNeedle) != std::string::npos ||
                content.find(wideNeedle) != std::string::npos)
            {
                matched = true;
                break;
            }
            if (content.size() > 1024 * 1024)
                break;
        }

        ::CloseHandle(file);

        if (matched)
        {
            found = path;
            break;
        }
    } while (::FindNextFileW(find, &entry));

    ::FindClose(find);
    return found;
}

InstallResult InstallPrinterViaIpp(const std::string& serverHost, int ippPort, int printerIndex,
                                   const std::string& printerName,
                                   const InstallProgressFn& progress)
{
    InstallResult result;
    result.printerName = printerName;
    result.driverName = "Microsoft IPP Class Driver";

    // 上报进度并把同一条文本返回，供 result.steps 记录（保持日志与步骤一致）
    auto report = [&progress](const std::string& text) -> std::string {
        if (progress)
            progress(text);
        return text;
    };

    if (serverHost.empty() || printerName.empty())
    {
        result.message = "参数不完整（服务器地址或打印机名称为空）";
        result.steps.push_back("❌ 参数不完整");
        return result;
    }

    if (ippPort <= 0 || ippPort > 65535)
        ippPort = 631;

    if (printerIndex < 0)
        printerIndex = 0;

    // 服务端端点末段 = 该打印机的 RAW 端口号（9100 / 9101 …），
    // 与 /api/info 返回的 index 一致；/ipp/print 始终是首台别名。
    // 必须与服务端 ResolvePrinterIndex 保持同一套规则，否则多打印机场景下
    // 会把作业发到别的打印机上（打 EPSON 却从 Lenovo 出纸）。
    const std::string path = (printerIndex <= 0)
                                 ? std::string("/ipp/print")
                                 : ("/ipp/print/" + std::to_string(printerIndex));

    const std::string url = "http://" + serverHost + ":" + std::to_string(ippPort) + path;
    result.portName = url;

    // ---- 1) 系统必须具备 IPP 类驱动（Win10 1809+ 自带；Win7/2008 没有）----
    if (!IsPrinterDriverInstalled(Utf8ToWide(result.driverName)))
    {
        result.message = "本机没有 Microsoft IPP Class Driver（需 Windows 10 1809 及以上）"
                         "，请改用普通驱动安装方式";
        result.steps.push_back(report("❌ 系统缺少 Microsoft IPP Class Driver"));
        return result;
    }
    result.steps.push_back(report("✅ 系统自带 Microsoft IPP Class Driver"));

    std::wstring infPath = FindIppClassDriverInf();
    if (infPath.empty())
    {
        result.message = "未找到 IPP 类驱动的 INF（%SystemRoot%\\INF\\prnms*.inf）";
        result.steps.push_back(report("❌ 未找到 IPP 类驱动 INF"));
        return result;
    }
    result.steps.push_back(report("ℹ 驱动 INF: " + WideToUtf8(infPath.substr(infPath.find_last_of(L'\\') + 1))));

    // ---- 2) 已经存在同名打印机时的处理 ----
    if (PrinterExists(printerName) || PrinterExistsByApi(printerName))
    {
        std::string existingPort;
        if (GetPrinterPortNameByApi(printerName, existingPort) &&
            _stricmp(existingPort.c_str(), url.c_str()) == 0)
        {
            result.success = true;
            result.message = "打印机已存在且已指向本 IPP 服务，无需重复安装";
            result.steps.push_back("ℹ 已存在同配置打印机，跳过创建");
            return result;
        }

        result.steps.push_back(report("ℹ 发现同名打印机（端口不同），先移除再重建"));

        CommandResult removeResult;
        std::wstring removeScript = L"-NoProfile -ExecutionPolicy Bypass -Command \"Remove-Printer -Name '" +
                                    Utf8ToWide(printerName) + L"' -ErrorAction SilentlyContinue\"";
        RunCommand(L"powershell.exe", removeScript, 60000, removeResult);

        // 回读确认真的移除了，否则后面会建出重名条目
        if (PrinterExists(printerName) || PrinterExistsByApi(printerName))
        {
            result.message = "已存在同名打印机且无法自动移除，请先在「打印机和扫描仪」里删除它再重试";
            result.steps.push_back(report("❌ 同名打印机无法移除"));
            return result;
        }
    }

    // ---- 3) 建队列：IPP 类驱动 + URL 端口 ----
    // 端口名就是 URL，这是 Internet Print Provider 的约定（本机实测可行）。
    result.steps.push_back(report("   IPP 地址: " + url));

    std::wstring arguments = L"printui.dll,PrintUIEntry /if /q /b \"" + Utf8ToWide(printerName) +
                             L"\" /f \"" + infPath + L"\" /r \"" + Utf8ToWide(url) +
                             L"\" /m \"Microsoft IPP Class Driver\"";

    result.steps.push_back(report("正在创建 IPP 免驱打印机..."));

    CommandResult command;
    RunCommand(L"rundll32.exe", arguments, 120000, command);

    // rundll32 退出码恒为 0、不可信 —— 一律回读系统状态判定
    if (!PrinterExists(printerName) && !PrinterExistsByApi(printerName))
    {
        result.message = "IPP 免驱打印机创建失败（系统里没有出现该打印机）";
        if (!command.output.empty())
            result.steps.push_back("  输出: " + TrimAscii(command.output));
        result.steps.push_back(report("❌ 创建失败"));
        return result;
    }

    std::string actualPort;
    if (GetPrinterPortNameByApi(printerName, actualPort) &&
        _stricmp(actualPort.c_str(), url.c_str()) != 0)
    {
        result.message = "打印机已创建，但端口不是 IPP 地址（实际: " + actualPort + "）";
        result.steps.push_back(report("⚠ 端口不匹配: " + actualPort));
        return result;
    }

    result.success = true;
    result.message = "IPP 免驱安装成功（使用系统自带 Microsoft IPP Class Driver，未安装厂商驱动）";
    result.steps.push_back(report("✅ 已创建打印机: " + printerName));
    result.steps.push_back(report("✅ IPP 端口: " + url));
    result.steps.push_back("ℹ 打印时由服务端把文档渲染后交给本机已装驱动输出");
    return result;
}

} // namespace PrinterDriverInstaller
} // namespace ps
