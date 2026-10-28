#include "pch.h"
#include "ErrorHandler.h"
#include "LogHelper.h"
#include "Wmi.h"

#include <winsock2.h>

namespace ps
{
namespace ErrorHandler
{

namespace
{
std::string FormatMessageW32(DWORD code)
{
    LPWSTR buffer = nullptr;
    DWORD len = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&buffer, 0, nullptr);

    std::string result;
    if (len > 0 && buffer != nullptr)
    {
        std::wstring text(buffer, len);
        // 去掉系统消息末尾的换行
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
            text.pop_back();
        result = WideToUtf8(text);
    }

    if (buffer != nullptr)
        ::LocalFree(buffer);

    return result;
}
}

void LogException(const std::string& context, const std::string& errorType,
                  const std::string& message, const std::string& stack)
{
    // 与 C# 格式一致：
    // [错误] 上下文: {0} | 类型: {1} | 消息: {2} | 堆栈: {3}
    std::string line = "[错误] 上下文: " + context +
                       " | 类型: " + errorType +
                       " | 消息: " + message +
                       " | 堆栈: " + (stack.empty() ? "(无堆栈)" : stack);

    Log::WriteLog(line);
}

void LogError(const std::string& context, const std::string& message)
{
    LogException(context, "Error", message, "");
}

std::string GetUserFriendlyMessage(unsigned long win32Error)
{
    switch (win32Error)
    {
    case WSAECONNREFUSED:
    case WSAETIMEDOUT:
    case WSAEHOSTUNREACH:
    case WSAENETUNREACH:
    case WSAENETDOWN:
    case ERROR_CONNECTION_REFUSED:
        return "网络连接失败，请检查网络设置";

    case ERROR_ACCESS_DENIED:
        return "权限不足，请以管理员身份运行";

    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_SHARING_VIOLATION:
    case ERROR_WRITE_PROTECT:
        return "文件读写失败: " + FormatMessageW32(win32Error);

    case WAIT_TIMEOUT:
    case ERROR_TIMEOUT:
        return "操作超时，请重试";

    default:
        break;
    }

    std::string text = FormatMessageW32(win32Error);
    return text.empty() ? ("错误代码: " + std::to_string(win32Error)) : text;
}

std::string GetLastErrorMessage()
{
    return GetUserFriendlyMessage(::GetLastError());
}

}
}
