#include "pch.h"
#include "Program.h"

#include <string>
#include <vector>

#include <winsvc.h>

#include "LogHelper.h"
#include "Wmi.h"

namespace ps
{
namespace ProgramHost
{

namespace
{

// 对应 C# 的 ProcessStartInfo{Verb="runas", UseShellExecute=true} + WaitForExit
// 返回 false 表示进程未能启动（用户在 UAC 对话框点「否」时 lastError = ERROR_CANCELLED）
bool ShellRunAndWait(const std::wstring& file, const std::wstring& parameters,
                     bool elevate, DWORD& exitCode, DWORD& lastError)
{
    exitCode = 0;
    lastError = ERROR_SUCCESS;

    SHELLEXECUTEINFOW info;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = elevate ? L"runas" : L"open";
    info.lpFile = file.c_str();
    info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&info))
    {
        lastError = ::GetLastError();
        return false;
    }

    if (info.hProcess != nullptr)
    {
        ::WaitForSingleObject(info.hProcess, INFINITE);
        ::GetExitCodeProcess(info.hProcess, &exitCode);
        ::CloseHandle(info.hProcess);
    }

    return true;
}

// 对应 C# 的 ProcessStartInfo{Verb="runas", UseShellExecute=true} + Process.Start：
// 只启动进程，不等待其退出（C# 的 RestartAsAdmin / LaunchTrayHelper 均不等待）
bool ShellRunNoWait(const std::wstring& file, const std::wstring& parameters,
                    bool elevate, DWORD& lastError)
{
    lastError = ERROR_SUCCESS;

    SHELLEXECUTEINFOW info;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.lpVerb = elevate ? L"runas" : L"open";
    info.lpFile = file.c_str();
    info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&info))
    {
        lastError = ::GetLastError();
        return false;
    }

    return true;
}

std::wstring FormatSystemError(DWORD code)
{
    // 对应 .NET Win32Exception.Message：取系统消息表里的描述文本
    LPWSTR buffer = nullptr;
    DWORD len = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                                 nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                 (LPWSTR)&buffer, 0, nullptr);

    std::wstring text;
    if (len != 0 && buffer != nullptr)
    {
        text.assign(buffer, len);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
            text.pop_back();
    }
    else
    {
        text = L"错误码 " + std::to_wstring(code);
    }

    if (buffer != nullptr)
        ::LocalFree(buffer);

    return text;
}

} // namespace

namespace
{
// 服务名与托盘助手窗口标题（实现细节，不对外暴露）
const wchar_t* kServiceName = L"PrintServer";
const wchar_t* kTrayWindowTitle = L"PrintServerTray";
} // namespace

// ===========================================================================
// 服务状态查询 / 等待（SCM API）
// ===========================================================================

ServiceState QueryServiceState()
{
    SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr)
        return ServiceState::Unknown;

    // 注意：这里必须“查询完立即关闭句柄”。SCM 只有在服务已停止且所有服务句柄都关闭后，
    // 才会把已标记删除的服务真正移除；长期持有句柄会让卸载永远停在“已标记为删除”。
    SC_HANDLE service = ::OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
    if (service == nullptr)
    {
        ::CloseServiceHandle(manager);
        return ServiceState::NotInstalled;
    }

    SERVICE_STATUS_PROCESS status;
    memset(&status, 0, sizeof(status));
    DWORD needed = 0;

    ServiceState state = ServiceState::Unknown;
    if (::QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                               reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed))
    {
        switch (status.dwCurrentState)
        {
        case SERVICE_STOPPED:       state = ServiceState::Stopped; break;
        case SERVICE_START_PENDING: state = ServiceState::StartPending; break;
        case SERVICE_STOP_PENDING:  state = ServiceState::StopPending; break;
        case SERVICE_RUNNING:       state = ServiceState::Running; break;
        case SERVICE_PAUSE_PENDING:
        case SERVICE_PAUSED:
        case SERVICE_CONTINUE_PENDING: state = ServiceState::Paused; break;
        default:                    state = ServiceState::Unknown; break;
        }
    }

    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    return state;
}

bool WaitForServiceStopped(int timeoutMs)
{
    const DWORD step = 200;
    for (int waited = 0; ; waited += (int)step)
    {
        const ServiceState state = QueryServiceState();

        if (state == ServiceState::Stopped || state == ServiceState::NotInstalled)
            return true;

        if (waited >= timeoutMs)
            return false;

        ::Sleep(step);
    }
}

bool WaitForServiceRemoved(int timeoutMs)
{
    const DWORD step = 200;
    for (int waited = 0; ; waited += (int)step)
    {
        if (QueryServiceState() == ServiceState::NotInstalled)
            return true;

        if (waited >= timeoutMs)
            return false;

        ::Sleep(step);
    }
}

bool RequestTrayHelperExit(int timeoutMs)
{
    HWND tray = ::FindWindowW(nullptr, kTrayWindowTitle);
    if (tray == nullptr)
        return false;

    ::PostMessageW(tray, WM_CLOSE, 0, 0);

    const DWORD step = 100;
    for (int waited = 0; waited < timeoutMs; waited += (int)step)
    {
        ::Sleep(step);
        if (::FindWindowW(nullptr, kTrayWindowTitle) == nullptr)
            return true;
    }

    return false;
}

std::wstring GetExecutablePath()
{
    wchar_t buffer[MAX_PATH * 2] = { 0 };
    DWORD len = ::GetModuleFileNameW(nullptr, buffer, _countof(buffer));
    return std::wstring(buffer, len);
}

std::string GetDisplayVersion()
{
    std::wstring path = GetExecutablePath();

    DWORD handle = 0;
    DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0)
        return "1.4.3";

    std::vector<unsigned char> data(size);
    if (!::GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return "1.4.3";

    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedLen = 0;
    if (!::VerQueryValueW(data.data(), L"\\", (LPVOID*)&fixed, &fixedLen) || fixed == nullptr)
        return "1.4.3";

    int major = (int)HIWORD(fixed->dwFileVersionMS);
    int minor = (int)LOWORD(fixed->dwFileVersionMS);
    int build = (int)HIWORD(fixed->dwFileVersionLS);
    int revision = (int)LOWORD(fixed->dwFileVersionLS);

    char buffer[64] = { 0 };
    // 对应 C#：ver.Revision <= 0 时只显示 3 段
    if (revision <= 0)
        _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%d.%d.%d", major, minor, build);
    else
        _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%d.%d.%d.%d", major, minor, build, revision);

    return buffer;
}

std::string GetDisplayVersionWithPrefix()
{
    return "v" + GetDisplayVersion();
}

bool IsAdministrator()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    TOKEN_ELEVATION elevation;
    memset(&elevation, 0, sizeof(elevation));
    DWORD returned = 0;

    bool result = false;
    if (::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned))
        result = (elevation.TokenIsElevated != 0);

    ::CloseHandle(token);
    return result;
}

void RestartAsAdmin(const std::vector<std::wstring>& args)
{
    std::wstring exePath = GetExecutablePath();

    std::wstring joined;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i > 0)
            joined += L" ";
        joined += args[i];
    }

    DWORD lastError = ERROR_SUCCESS;

    // 对应 C# RestartAsAdmin：启动提权副本后立即返回，不等待其退出
    if (ShellRunNoWait(exePath, joined, true, lastError))
        return;

    if (lastError == ERROR_CANCELLED)
    {
        // 用户在 UAC 对话框点击了「否」
        ::MessageBoxW(nullptr, L"打印服务器需要管理员权限才能运行，请允许UAC提权。",
                      L"权限不足", MB_OK | MB_ICONWARNING);
    }
    else
    {
        wchar_t message[512] = { 0 };
        _snwprintf_s(message, _countof(message), _TRUNCATE,
                     L"提权失败: 错误码 %lu", lastError);
        ::MessageBoxW(nullptr, message, L"错误", MB_OK | MB_ICONERROR);
    }
}

void SetTrayAutoStart(bool enable)
{
    HKEY key = nullptr;
    LONG status = ::RegOpenKeyExW(HKEY_CURRENT_USER,
                                  L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                                  0, KEY_SET_VALUE | KEY_QUERY_VALUE, &key);

    if (status != ERROR_SUCCESS || key == nullptr)
        return;

    if (enable)
    {
        std::wstring command = L"\"" + GetExecutablePath() + L"\" /tray";

        status = ::RegSetValueExW(key, L"PrintServerTray", 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(command.c_str()),
                                  (DWORD)((command.size() + 1) * sizeof(wchar_t)));

        if (status == ERROR_SUCCESS)
            Log::WriteLog("[Setup] 托盘助手已添加到开机自启");
        else
            Log::WriteLog("[Setup] 托盘自启设置失败: 写入注册表失败（错误码 " +
                          std::to_string(status) + "）");
    }
    else
    {
        status = ::RegDeleteValueW(key, L"PrintServerTray");
        if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND)
            Log::WriteLog("[Setup] 托盘助手已从开机自启移除");
        else
            Log::WriteLog("[Setup] 托盘自启设置失败: 删除注册表值失败（错误码 " +
                          std::to_string(status) + "）");
    }

    ::RegCloseKey(key);
}

void LaunchTrayHelper()
{
    std::wstring exePath = GetExecutablePath();

    DWORD lastError = ERROR_SUCCESS;

    // 对应 C# LaunchTrayHelper：即发即忘，不得阻塞调用线程（否则安装服务成功后界面会卡死）
    if (!ShellRunNoWait(exePath, L"/tray", false, lastError))
    {
        Log::WriteLog("[Setup] 启动托盘助手失败: " + ps::WideToUtf8(FormatSystemError(lastError)));
    }
}

// ===========================================================================
// 服务安装 / 卸载
// ===========================================================================

void InstallService()
{
    std::wstring path = GetExecutablePath();

    // 严格对应 C# 传给 sc 的参数串（binPath= / start= 后的空格不可省略）
    std::wstring createArgs =
        L"create PrintServer binPath= \"\\\"" + path +
        L"\\\" /service\" start= auto DisplayName= \"网络打印服务器\"";

    DWORD exitCode = 0;
    DWORD lastError = ERROR_SUCCESS;

    if (!ShellRunAndWait(L"sc", createArgs, true, exitCode, lastError))
    {
        std::wstring message = L"安装服务时出错：" + FormatSystemError(lastError);
        ::MessageBoxW(nullptr, message.c_str(), L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    if (exitCode == 0)
    {
        ShellRunAndWait(L"sc", L"description PrintServer \"将本地打印机共享为网络打印机\"",
                        false, exitCode, lastError);
        ShellRunAndWait(L"sc", L"start PrintServer", false, exitCode, lastError);

        SetTrayAutoStart(true);
        LaunchTrayHelper();

        ::MessageBoxW(nullptr,
                      L"服务安装成功并已启动！\n托盘助手已启用，可最小化到系统托盘监控服务状态。",
                      L"成功", MB_OK | MB_ICONINFORMATION);
    }
    else
    {
        ::MessageBoxW(nullptr, L"服务安装失败，请以管理员身份运行！", L"错误",
                      MB_OK | MB_ICONERROR);
    }
}

void UninstallService()
{
    DWORD exitCode = 0;
    DWORD lastError = ERROR_SUCCESS;

    // 托盘助手持有服务句柄，会让删除只停在“已标记为删除”，必须先请它退出
    RequestTrayHelperExit(3000);

    ShellRunAndWait(L"sc", L"stop PrintServer", false, exitCode, lastError);
    WaitForServiceStopped(15000);

    SetTrayAutoStart(false);

    if (!ShellRunAndWait(L"sc", L"delete PrintServer", true, exitCode, lastError))
    {
        std::wstring message = L"卸载服务时出错：" + FormatSystemError(lastError);
        ::MessageBoxW(nullptr, message.c_str(), L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    // sc delete 成功只代表“已下达删除”；再用 SCM 确认服务条目是否真的消失
    if (WaitForServiceRemoved(8000))
    {
        ::MessageBoxW(nullptr, L"服务卸载成功！", L"成功", MB_OK | MB_ICONINFORMATION);
    }
    else
    {
        ::MessageBoxW(nullptr,
                      L"服务已停止并标记为删除，但尚未从服务列表移除。\n\n"
                      L"如有其它进程（如托盘监控）仍在使用该服务，关闭后会立即生效；\n"
                      L"也可以重启计算机后确认。",
                      L"提示", MB_OK | MB_ICONINFORMATION);
    }
}

} // namespace ProgramHost
} // namespace ps
