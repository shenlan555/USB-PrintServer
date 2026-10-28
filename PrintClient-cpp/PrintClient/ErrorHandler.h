#pragma once

// 统一错误处理辅助类（对应 C# ErrorHandler.cs）
//
// C# 侧依赖异常类型判断，C++ 侧用 ExceptionKind 枚举等价表达。

#include <string>

namespace ps
{
namespace ErrorHandler
{

// 对应 ErrorHandler.cs 里被判断的异常类型
enum class ExceptionKind
{
    Socket,             // System.Net.Sockets.SocketException
    HttpRequest,        // System.Net.Http.HttpRequestException
    Web,                // System.Net.WebException
    UnauthorizedAccess, // System.UnauthorizedAccessException
    IO,                 // System.IO.IOException
    Timeout,            // System.TimeoutException
    Other
};

// 对应 ErrorHandler.FormatLogMessage(ex, context)
// 输出："[错误] 上下文: {context} | 类型: {typeName} | 消息: {message}"
std::string FormatLogMessage(const std::string& context, const std::string& typeName,
                             const std::string& message);

// 对应 ErrorHandler.GetUserFriendlyMessage(ex)
std::string GetUserFriendlyMessage(ExceptionKind kind, const std::string& message);

} // namespace ErrorHandler
} // namespace ps
