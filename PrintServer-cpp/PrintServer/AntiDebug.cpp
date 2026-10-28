#include "pch.h"
#include "AntiDebug.h"
#include "BuildConfig.h"
#include "LicenseHelper.h"
#include "MachineCode.h"
#include "Crypto.h"
#include "LogHelper.h"
#include "Config.h"
#include "Json.h"
#include "Wmi.h"

#include <tlhelp32.h>
#include <winhttp.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <thread>
#include <random>
#include <chrono>

#pragma comment(lib, "winhttp.lib")

namespace ps
{
namespace AntiDebug
{

// ===========================================================================
// 原生 API
// ===========================================================================

namespace
{

using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

const ULONG kProcessDebugPort = 7;
const ULONG kProcessDebugObjectHandle = 30;
const ULONG kProcessDebugFlags = 31;

NtQueryInformationProcessFn GetNtQueryInformationProcess()
{
    static NtQueryInformationProcessFn s_fn = nullptr;
    static bool s_resolved = false;
    if (!s_resolved)
    {
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll != nullptr)
            s_fn = (NtQueryInformationProcessFn)::GetProcAddress(ntdll, "NtQueryInformationProcess");
        s_resolved = true;
    }
    return s_fn;
}

// ---------------------------------------------------------------------------
// 全局状态（对应 C# 的静态字段）
// ---------------------------------------------------------------------------

std::atomic<bool> g_penaltyTriggered(false);
std::atomic<int> g_detectionCount(0);
std::atomic<bool> g_guardianRunning(false);
std::thread g_guardianThread;
std::chrono::steady_clock::time_point g_lastCheckTime = (std::chrono::steady_clock::time_point::min)();
std::mutex g_checkMutex;

const int kDetectionThreshold = 3;
const int kCheckIntervalMs = 30000;  // 两次检测间隔至少 30 秒

const wchar_t* kRegistryPath = L"Software\\PrintServer";
const wchar_t* kRegistryBlacklistKey = L"Blacklisted";

std::mt19937& GetRng()
{
    static std::mt19937 s_rng{ std::random_device{}() };
    return s_rng;
}

// ---------------------------------------------------------------------------
// 检测方法
// ---------------------------------------------------------------------------

bool CheckDebuggerPresent()
{
    if (::IsDebuggerPresent())
        return true;

    BOOL remote = FALSE;
    if (::CheckRemoteDebuggerPresent(::GetCurrentProcess(), &remote) && remote)
        return true;

    return false;
}

bool CheckDebugPort()
{
    NtQueryInformationProcessFn fn = GetNtQueryInformationProcess();
    if (fn == nullptr)
        return false;

    ULONG_PTR debugPort = 0;
    ULONG returnLength = 0;
    LONG status = fn(::GetCurrentProcess(), kProcessDebugPort, &debugPort,
                     (ULONG)sizeof(debugPort), &returnLength);
    return (status == 0 && debugPort != 0);
}

bool CheckDebugObjectHandle()
{
    NtQueryInformationProcessFn fn = GetNtQueryInformationProcess();
    if (fn == nullptr)
        return false;

    ULONG_PTR handle = 0;
    ULONG returnLength = 0;
    LONG status = fn(::GetCurrentProcess(), kProcessDebugObjectHandle, &handle,
                     (ULONG)sizeof(handle), &returnLength);
    return (status == 0 && handle != 0);
}

bool CheckDebugFlags()
{
    NtQueryInformationProcessFn fn = GetNtQueryInformationProcess();
    if (fn == nullptr)
        return false;

    ULONG_PTR flags = 0;
    ULONG returnLength = 0;
    LONG status = fn(::GetCurrentProcess(), kProcessDebugFlags, &flags,
                     (ULONG)sizeof(flags), &returnLength);
    // 有调试器时该值为 0
    return (status == 0 && flags == 0);
}

// 硬件断点检测（跳过当前线程，避免死锁）
//
// 注意：C# 原版自建的 CONTEXT 结构与 x64 真实布局不符（ContextFlags/Dr0 偏移错误、
//       结构体尺寸仅 544 字节而实际约 1232 字节），GetThreadContext 会越界写入内存。
//       此处改用 winnt.h 的真实 CONTEXT 与正确的上下文标志，修复该缺陷。
bool CheckHardwareBreakpoints()
{
    DWORD currentThreadId = ::GetCurrentThreadId();
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    THREADENTRY32 te = { 0 };
    te.dwSize = sizeof(te);

    DWORD processId = ::GetCurrentProcessId();
    bool detected = false;

    if (::Thread32First(snapshot, &te))
    {
        do
        {
            if (te.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(DWORD))
                continue;
            if (te.th32OwnerProcessID != processId)
                continue;
            if (te.th32ThreadID == currentThreadId)
                continue;

            HANDLE hThread = ::OpenThread(
                THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (hThread == nullptr)
                continue;

            CONTEXT ctx = { 0 };
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (::GetThreadContext(hThread, &ctx))
            {
                if (ctx.Dr0 != 0 || ctx.Dr1 != 0 || ctx.Dr2 != 0 || ctx.Dr3 != 0)
                    detected = true;
                else if ((ctx.Dr7 & 0xFF) != 0)
                    detected = true;
            }

            ::CloseHandle(hThread);

            if (detected)
                break;
        } while (::Thread32Next(snapshot, &te));
    }

    ::CloseHandle(snapshot);
    return detected;
}

bool CheckDebuggerModules()
{
    static const wchar_t* kSuspiciousModules[] = {
        L"scylla.dll", L"scyllahide.dll",
        L"hook.dll", L"inject.dll", L"vmt.dll",
        L"sharpod.dll", L"x64dbg.dll", L"x32dbg.dll",
        L"ollydbg.dll", L"strongod.dll", L"phnt.dll"
    };

    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                                 ::GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    MODULEENTRY32W me = { 0 };
    me.dwSize = sizeof(me);

    bool detected = false;
    if (::Module32FirstW(snapshot, &me))
    {
        do
        {
            std::string name = WideToUtf8(me.szModule);
            for (char& c : name)
            {
                if (c >= 'A' && c <= 'Z')
                    c = (char)(c - 'A' + 'a');
            }

            for (const wchar_t* suspicious : kSuspiciousModules)
            {
                if (name == WideToUtf8(suspicious))
                {
                    detected = true;
                    break;
                }
            }

            if (detected)
                break;
        } while (::Module32NextW(snapshot, &me));
    }

    ::CloseHandle(snapshot);
    return detected;
}

bool CheckDebuggerProcesses()
{
    // 对应 C#：进程名 Contains(调试器名)（子串匹配）
    static const char* kDebuggerProcesses[] = {
        "x64dbg", "x32dbg", "x96dbg", "ollydbg",
        "ida", "ida64", "windbg", "windbg64",
        "dnspy", "cheatengine",
        "immunitydebugger"
    };

    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W pe = { 0 };
    pe.dwSize = sizeof(pe);

    bool detected = false;
    if (::Process32FirstW(snapshot, &pe))
    {
        do
        {
            std::string name = WideToUtf8(pe.szExeFile);
            for (char& c : name)
            {
                if (c >= 'A' && c <= 'Z')
                    c = (char)(c - 'A' + 'a');
            }

            for (const char* debugger : kDebuggerProcesses)
            {
                if (name.find(debugger) != std::string::npos)
                {
                    detected = true;
                    break;
                }
            }

            if (detected)
                break;
        } while (::Process32NextW(snapshot, &pe));
    }

    ::CloseHandle(snapshot);
    return detected;
}

// ---------------------------------------------------------------------------
// 惩罚措施
// ---------------------------------------------------------------------------

void LogSecurityAlert(const std::string& location)
{
    // 与 C# 一致：写到 %LocalAppData%\PrintServer\security.log
    PWSTR localAppData = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)))
        return;

    std::wstring dir = std::wstring(localAppData) + L"\\PrintServer";
    ::CoTaskMemFree(localAppData);

    ::CreateDirectoryW(dir.c_str(), nullptr);

    SYSTEMTIME st;
    ::GetLocalTime(&st);

    // 用宽字符构造后显式转 UTF-8，避免依赖执行字符集（与 C# 写 UTF-8 的行为一致）
    wchar_t wideLine[512] = { 0 };
    _snwprintf_s(wideLine, _countof(wideLine), _TRUNCATE,
                 L"[SECURITY] %04d-%02d-%02d %02d:%02d:%02d - 反调试触发 - %s\r\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 Utf8ToWide(location).c_str());

    std::string line = WideToUtf8(wideLine);

    std::wstring path = dir + L"\\security.log";
    HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        ::WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
        ::CloseHandle(h);
    }
}

void DeleteLicenseFile()
{
    // 对应 C#：先用随机数据覆盖再删除
    std::wstring path = License::Internal::LicenseFilePath();
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;

    std::vector<unsigned char> garbage(1024);
    for (auto& b : garbage)
        b = (unsigned char)(GetRng()() & 0xFF);

    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        ::WriteFile(h, garbage.data(), (DWORD)garbage.size(), &written, nullptr);
        ::CloseHandle(h);
    }

    ::DeleteFileW(path.c_str());
}

void WriteBlacklistToRegistry()
{
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr,
                          REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    {
        return;
    }

    DWORD one = 1;
    ::RegSetValueExW(key, kRegistryBlacklistKey, 0, REG_DWORD, (const BYTE*)&one, sizeof(one));

    SYSTEMTIME st;
    ::GetLocalTime(&st);
    wchar_t timeText[32] = { 0 };
    _snwprintf_s(timeText, _countof(timeText), _TRUNCATE,
                 L"%04d-%02d-%02d %02d:%02d:%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    ::RegSetValueExW(key, L"TamperTime", 0, REG_SZ,
                     (const BYTE*)timeText, (DWORD)((wcslen(timeText) + 1) * sizeof(wchar_t)));

    ::RegCloseKey(key);
}

void ReportTamperingToServer()
{
    // 对应 C#：后台线程 POST report_tamper
    std::thread([]() {
        try
        {
            JsonValue data = JsonValue::Object();
            data.Set("machine_code", JsonValue::Str(MachineCode::GenerateMachineCode()));
            data.Set("license_key", JsonValue::Str(License::CurrentLicenseKey()));

            std::string hostname = WideToUtf8([]() -> std::wstring {
                wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
                DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
                ::GetComputerNameW(buf, &len);
                return std::wstring(buf, len);
            }());
            data.Set("hostname", JsonValue::Str(hostname));

            data.Set("action", JsonValue::Str("tamper_report"));

            SYSTEMTIME st;
            ::GetLocalTime(&st);
            char ts[32] = { 0 };
            _snprintf_s(ts, sizeof(ts), _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d",
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            data.Set("timestamp", JsonValue::Str(ts));

            std::string url = Crypto::DecodeLicenseUrl();
            License::Internal::HttpPost(url + "report_tamper", data.Serialize(), 10);
        }
        catch (...)
        {
        }
    }).detach();
}

void ExecutePenalty()
{
    bool expected = false;
    if (!g_penaltyTriggered.compare_exchange_strong(expected, true))
        return; // 只执行一次

    StopGuardian();

    // 1. 删除授权文件
    DeleteLicenseFile();

    // 2. 写入注册表黑名单
    WriteBlacklistToRegistry();

    // 3. 联网上报服务端（异步）
    ReportTamperingToServer();

    // 4. 随机延迟 10~30 秒
    std::uniform_int_distribution<int> delayDist(10, 30);
    int delaySeconds = delayDist(GetRng());
    std::this_thread::sleep_for(std::chrono::seconds(delaySeconds));

    // 5. 随机选择惩罚方式
    std::uniform_int_distribution<int> typeDist(1, 4);
    int penaltyType = typeDist(GetRng());

    switch (penaltyType)
    {
    case 1:
        ::MessageBoxW(nullptr, L"系统检测到异常环境，程序已退出", L"安全提示",
                      MB_OK | MB_ICONWARNING);
        ::ExitProcess(0);
        break;
    case 2:
        ::MessageBoxW(nullptr, L"程序遇到未知错误，即将关闭", L"错误",
                      MB_OK | MB_ICONERROR);
        ::ExitProcess(1);
        break;
    default:
        ::ExitProcess(0);
        break;
    }
}

// ---------------------------------------------------------------------------
// 守护线程
// ---------------------------------------------------------------------------

void GuardianLoop()
{
    while (g_guardianRunning.load())
    {
        // 分段睡眠，保证能及时响应停止请求
        for (int i = 0; i < 30 && g_guardianRunning.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));

        if (!g_guardianRunning.load())
            break;

        // 1. 反调试检测
        if (CheckAll())
        {
            LogSecurityAlert("GuardianThread");
            ExecutePenalty();
        }

        // 2. 授权系统篡改检测
        if (IsLicenseSystemTampered())
        {
            LogSecurityAlert("LicenseSystemTampered");
            ExecutePenalty();
        }
    }
}

} // namespace

// ===========================================================================
// 对外接口
// ===========================================================================

// ===== 开源免费版：反调试/惩罚机制整体关闭（BuildConfig::LicenseEnabled = false）=====
// 该机制原本服务于商业授权保护；免费开源版不再对用户环境做任何检测与惩罚，
// 所有入口短路，原实现保留以便通过 BuildConfig::LicenseEnabled 恢复。

bool CheckAll()
{
    if (!BuildConfig::LicenseEnabled)
        return false; // 永不判定为“检测到调试器”

    if (g_penaltyTriggered.load())
        return true;

    std::lock_guard<std::mutex> guard(g_checkMutex);

    // 频率限制：两次检测之间至少间隔 30 秒
    auto now = std::chrono::steady_clock::now();
    if (g_lastCheckTime != (std::chrono::steady_clock::time_point::min)())
    {
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastCheckTime).count();
        if (elapsedMs < kCheckIntervalMs)
            return false;
    }
    g_lastCheckTime = now;

    bool detected = false;

    if (CheckDebuggerPresent())     detected = true;
    if (CheckDebugPort())           detected = true;
    if (CheckDebugObjectHandle())   detected = true;
    if (CheckDebugFlags())          detected = true;
    if (CheckHardwareBreakpoints()) detected = true;
    if (CheckDebuggerModules())     detected = true;
    if (CheckDebuggerProcesses())   detected = true;

    if (detected)
    {
        int count = g_detectionCount.load() + 1;
        g_detectionCount.store(count);
        if (count >= kDetectionThreshold)
            return true;
    }
    else
    {
        if (g_detectionCount.load() > 0)
            g_detectionCount.fetch_sub(1);
    }

    return false;
}

void Checkpoint(const std::string& location)
{
    if (CheckAll())
    {
        LogSecurityAlert(location);
        ExecutePenalty();
    }
}

void StartGuardian()
{
    if (!BuildConfig::LicenseEnabled)
        return; // 免费版：不启动守护线程

    if (g_guardianRunning.exchange(true))
        return;

    g_guardianThread = std::thread(GuardianLoop);
    g_guardianThread.detach();
}

void StopGuardian()
{
    g_guardianRunning.store(false);
}

bool IsBlacklisted()
{
    if (!BuildConfig::LicenseEnabled)
        return false; // 免费版：无黑名单

    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;

    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    LONG result = ::RegQueryValueExW(key, kRegistryBlacklistKey, nullptr, &type,
                                     (BYTE*)&value, &size);
    ::RegCloseKey(key);

    return (result == ERROR_SUCCESS && value == 1);
}

void Initialize()
{
    if (!BuildConfig::LicenseEnabled)
        return; // 免费版：不做黑名单检查、不启动守护线程

    // 检查注册表黑名单
    if (IsBlacklisted())
    {
        LogSecurityAlert("BlacklistCheck");
        ::ExitProcess(0);
    }

    // 启动守护线程
    StartGuardian();
}

void ForcePenalty()
{
    if (!BuildConfig::LicenseEnabled)
        return; // 免费版：永不执行惩罚

    ExecutePenalty();
}

bool IsLicenseSystemTampered()
{
    // 忠实复刻 C# 原版实现：两个分支都返回 false，
    // 因此"授权系统篡改检测"在当前版本中实际不生效（保留结构以便后续补全）。
    if (License::CurrentStatus() == License::Status::Unknown &&
        License::HasLocalLicenseFile())
    {
        return false;
    }

    return false;
}

}
}
