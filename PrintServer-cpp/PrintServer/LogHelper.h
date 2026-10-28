#pragma once

// 日志辅助（对应 C# LogHelper.cs）
//
// 日志按天分文件：log\printserver-yyyy-MM-dd.log
// 过期清理天数读取 App.config 的 LogRetentionDays（默认 15）。

#include <string>
#include <vector>
#include <windows.h>

namespace ps
{
namespace Log
{
    std::wstring GetLogDirectory();
    std::wstring GetTodayLogPath();
    std::wstring GetLogPathByDate(const SYSTEMTIME& date);

    // 线程安全追加一行（自动补 CRLF），与 C# File.AppendAllText 行为一致
    void WriteLog(const std::string& messageUtf8);

    // 读取今天日志全文（已 Trim；与 C# 一致）
    std::string ReadTodayLog();

    // 今天日志行数（按 CRLF 切分，不做空行过滤）
    int GetTodayLogLineCount();

    // 最近 N 行
    std::vector<std::string> GetRecentLogLines(int maxLines);

    // 所有日志文件（按文件名排序）
    std::vector<std::wstring> GetAllLogFiles();

    // 清理超过保留天数的日志
    void CleanupOldLogs();

    // 按 CRLF 切分（与 C# String.Split(Environment.NewLine, None) 等价）
    std::vector<std::string> SplitLines(const std::string& content);
}
}
