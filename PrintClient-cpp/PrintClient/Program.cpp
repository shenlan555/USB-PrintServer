#include "pch.h"
#include "Program.h"

#include <string>
#include <vector>

#include "BuildConfig.h"

namespace ps
{
namespace ProgramHost
{

namespace
{

// 以 ShellExecuteEx 启动进程；elevate=true 时请求 UAC 提权（对应 Verb="runas"）
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

    if (info.hProcess != nullptr)
        ::CloseHandle(info.hProcess);

    return true;
}

// 把参数拼成命令行（与服务端 RestartAsAdmin 的做法一致：原样空格拼接）
std::wstring JoinArguments(const std::vector<std::wstring>& args)
{
    std::wstring joined;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i > 0)
            joined += L" ";
        joined += args[i];
    }
    return joined;
}

} // namespace

std::wstring GetExecutablePath()
{
    wchar_t buffer[MAX_PATH * 2] = { 0 };
    DWORD len = ::GetModuleFileNameW(nullptr, buffer, _countof(buffer));
    return std::wstring(buffer, len);
}

std::wstring GetExecutableDirectory()
{
    std::wstring path = GetExecutablePath();

    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return path;

    return path.substr(0, slash + 1);
}

std::string GetDisplayVersion()
{
    std::wstring path = GetExecutablePath();

    DWORD handle = 0;
    DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0)
        return BuildConfig::FallbackVersion;

    std::vector<unsigned char> data(size);
    if (!::GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return BuildConfig::FallbackVersion;

    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedLen = 0;
    if (!::VerQueryValueW(data.data(), L"\\", (LPVOID*)&fixed, &fixedLen) || fixed == nullptr)
        return BuildConfig::FallbackVersion;

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

bool RelaunchElevated(const std::vector<std::wstring>& args)
{
    DWORD lastError = ERROR_SUCCESS;

    // 启动提权副本后立即返回，不等待其退出（由调用方结束当前实例）
    if (ShellRunNoWait(GetExecutablePath(), JoinArguments(args), true, lastError))
        return true;

    if (lastError == ERROR_CANCELLED)
    {
        ::MessageBoxW(nullptr,
                      L"安装打印机驱动需要管理员权限，请在弹出的 UAC 提示中选择「是」。",
                      L"权限不足", MB_OK | MB_ICONWARNING);
    }
    else
    {
        wchar_t message[512] = { 0 };
        _snwprintf_s(message, _countof(message), _TRUNCATE,
                     L"提权失败: 错误码 %lu", lastError);
        ::MessageBoxW(nullptr, message, L"错误", MB_OK | MB_ICONERROR);
    }

    return false;
}

} // namespace ProgramHost
} // namespace ps
