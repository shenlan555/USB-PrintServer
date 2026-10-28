#pragma once

// 统一错误处理辅助（对应 C# ErrorHandler.cs）
//
// 说明：C# 版依据 .NET 异常类型（SocketException 等）给出提示，
//       原生版没有异常类型体系，改为依据 Win32 错误码映射，语义等价。
//       注意 C# 原版的 SafeExecute/GetUserFriendlyMessage 在项目中并未被调用
//       （属预留工具），此处按"功能不变"一并保留。

#include <string>
#include <windows.h>

namespace ps
{
namespace ErrorHandler
{
    // 对应 C# ErrorHandler.LogException
    void LogException(const std::string& context, const std::string& errorType,
                      const std::string& message, const std::string& stack);

    // 便捷重载：仅上下文与消息
    void LogError(const std::string& context, const std::string& message);

    // 对应 C# ErrorHandler.GetUserFriendlyMessage
    std::string GetUserFriendlyMessage(unsigned long win32Error);

    // 取最近一次 Win32 错误的友好提示
    std::string GetLastErrorMessage();
}
}
