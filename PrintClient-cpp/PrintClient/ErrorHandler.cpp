#include "pch.h"
#include "ErrorHandler.h"

#include <string>

namespace ps
{
namespace ErrorHandler
{

std::string FormatLogMessage(const std::string& context, const std::string& typeName,
                             const std::string& message)
{
    // 对应 string.Format("[错误] 上下文: {0} | 类型: {1} | 消息: {2}", ...)
    return "[错误] 上下文: " + context + " | 类型: " + typeName + " | 消息: " + message;
}

std::string GetUserFriendlyMessage(ExceptionKind kind, const std::string& message)
{
    switch (kind)
    {
    case ExceptionKind::Socket:
        return "网络连接失败，请检查网络设置";
    case ExceptionKind::HttpRequest:
        return "HTTP 请求失败，请检查服务器地址和端口";
    case ExceptionKind::Web:
        return "网络请求超时，请检查网络连接";
    case ExceptionKind::UnauthorizedAccess:
        return "权限不足，请以管理员身份运行";
    case ExceptionKind::IO:
        return "文件读写失败: " + message;
    case ExceptionKind::Timeout:
        return "操作超时，请重试";
    default:
        break;
    }

    return message;
}

} // namespace ErrorHandler
} // namespace ps
