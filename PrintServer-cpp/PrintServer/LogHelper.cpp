#include "pch.h"
#include "LogHelper.h"
#include "Config.h"

#include <mutex>
#include <algorithm>
#include <time.h>
#include <stdio.h>

namespace ps
{
namespace Log
{

namespace
{

std::mutex& GetLock()
{
    static std::mutex s_lock;
    return s_lock;
}

std::wstring FormatPath(const SYSTEMTIME& st)
{
    wchar_t name[64] = { 0 };
    _snwprintf_s(name, _countof(name), _TRUNCATE,
                 L"printserver-%04d-%02d-%02d.log",
                 st.wYear, st.wMonth, st.wDay);
    return std::wstring(GetLogDirectory()) + L"\\" + name;
}

// 首次调用时创建目录并执行清理（对应 C# 静态构造函数）
void EnsureInitialized()
{
    static bool s_initialized = false;
    static std::once_flag s_once;
    std::call_once(s_once, []() {
        std::wstring dir = GetLogDirectory();
        ::CreateDirectoryW(dir.c_str(), nullptr);
        CleanupOldLogs();
        s_initialized = true;
    });
}

std::string ReadFileUtf8(const std::wstring& path)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::string();

    LARGE_INTEGER size = { 0 };
    if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0)
    {
        ::CloseHandle(h);
        return std::string();
    }

    std::vector<char> buf((size_t)size.QuadPart);
    DWORD read = 0;
    ::ReadFile(h, buf.data(), (DWORD)buf.size(), &read, nullptr);
    ::CloseHandle(h);
    buf.resize(read);

    size_t start = 0;
    if (buf.size() >= 3 &&
        (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
    {
        start = 3;
    }

    return std::string(buf.begin() + start, buf.end());
}

std::string TrimAscii(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= 0x20)
        ++b;
    while (e > b && (unsigned char)s[e - 1] <= 0x20)
        --e;
    return s.substr(b, e - b);
}

// FILETIME(UTC) → time_t(UTC)
time_t FileTimeToTimeT(const FILETIME& ft)
{
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // 100ns 单位，起点 1601-01-01
    return (time_t)((uli.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

} // namespace

std::wstring GetLogDirectory()
{
    return Config::GetExeDirectory() + L"\\log";
}

std::wstring GetTodayLogPath()
{
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    return FormatPath(st);
}

std::wstring GetLogPathByDate(const SYSTEMTIME& date)
{
    return FormatPath(date);
}

void WriteLog(const std::string& messageUtf8)
{
    EnsureInitialized();

    std::lock_guard<std::mutex> guard(GetLock());

    std::wstring path = GetTodayLogPath();

    // 对应 C# File.AppendAllText：文件不存在(或为空)时写入 UTF-8 BOM
    bool needBom = true;
    WIN32_FILE_ATTRIBUTE_DATA attr = { 0 };
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
        needBom = (attr.nFileSizeHigh == 0 && attr.nFileSizeLow == 0);

    HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;

    if (needBom)
    {
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        ::WriteFile(h, bom, 3, &written, nullptr);
    }

    std::string line = messageUtf8 + "\r\n";
    ::WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);

    ::CloseHandle(h);
}

std::string ReadTodayLog()
{
    return TrimAscii(ReadFileUtf8(GetTodayLogPath()));
}

std::vector<std::string> SplitLines(const std::string& content)
{
    std::vector<std::string> lines;
    size_t pos = 0;

    while (true)
    {
        size_t eol = content.find("\r\n", pos);
        if (eol == std::string::npos)
        {
            lines.push_back(content.substr(pos));
            break;
        }
        lines.push_back(content.substr(pos, eol - pos));
        pos = eol + 2;
    }

    return lines;
}

int GetTodayLogLineCount()
{
    std::string content = ReadTodayLog();
    if (content.empty())
        return 0;
    return (int)SplitLines(content).size();
}

std::vector<std::string> GetRecentLogLines(int maxLines)
{
    std::string content = ReadTodayLog();
    if (content.empty() || maxLines <= 0)
        return std::vector<std::string>();

    std::vector<std::string> lines = SplitLines(content);
    if ((int)lines.size() <= maxLines)
        return lines;

    return std::vector<std::string>(lines.end() - maxLines, lines.end());
}

std::vector<std::wstring> GetAllLogFiles()
{
    std::vector<std::wstring> result;

    std::wstring pattern = GetLogDirectory() + L"\\printserver-*.log";

    WIN32_FIND_DATAW fd = { 0 };
    HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return result;

    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            result.push_back(GetLogDirectory() + L"\\" + fd.cFileName);
    } while (::FindNextFileW(hFind, &fd));

    ::FindClose(hFind);

    // 对应 C# Array.Sort(files, (a,b) => a.Name.CompareTo(b.Name))
    std::sort(result.begin(), result.end(),
              [](const std::wstring& a, const std::wstring& b) { return a < b; });

    return result;
}

void CleanupOldLogs()
{
    // 保留天数：读 App.config 的 LogRetentionDays，默认 15
    int retentionDays = 15;
    {
        std::string val = Config::GetSetting("LogRetentionDays", "15");
        if (!val.empty())
        {
            int parsed = atoi(val.c_str());
            if (parsed > 0)
                retentionDays = parsed;
        }
    }

    std::wstring dir = GetLogDirectory();
    if (::GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;

    time_t now = time(nullptr);
    time_t cutoff = now - (time_t)retentionDays * 24 * 3600;

    std::wstring pattern = dir + L"\\printserver-*.log";

    WIN32_FIND_DATAW fd = { 0 };
    HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        std::wstring name = fd.cFileName;
        bool deleted = false;

        // 对应 C#：name.Length >= 22 时取 Substring(12, 10) 解析日期
        if (name.size() >= 22)
        {
            std::wstring datePart = name.substr(12, 10); // "yyyy-MM-dd"

            int y = 0, m = 0, d = 0;
            bool parsed = false;
            if (swscanf_s(datePart.c_str(), L"%4d-%2d-%2d", &y, &m, &d) == 3)
            {
                if (y >= 1970 && m >= 1 && m <= 12 && d >= 1 && d <= 31)
                {
                    struct tm tmv = { 0 };
                    tmv.tm_year = y - 1900;
                    tmv.tm_mon = m - 1;
                    tmv.tm_mday = d;
                    tmv.tm_isdst = -1;
                    time_t fileDate = mktime(&tmv);
                    if (fileDate != (time_t)-1)
                    {
                        parsed = true;
                        if (fileDate < cutoff)
                        {
                            std::wstring full = dir + L"\\" + name;
                            if (::DeleteFileW(full.c_str()))
                                deleted = true;
                        }
                    }
                }
            }

            if (!parsed && !deleted)
            {
                if (FileTimeToTimeT(fd.ftLastWriteTime) < cutoff)
                {
                    std::wstring full = dir + L"\\" + name;
                    ::DeleteFileW(full.c_str());
                }
            }
        }
        else
        {
            // 不符合命名规则的旧文件，按最后修改时间清理
            if (FileTimeToTimeT(fd.ftLastWriteTime) < cutoff)
            {
                std::wstring full = dir + L"\\" + name;
                ::DeleteFileW(full.c_str());
            }
        }
    } while (::FindNextFileW(hFind, &fd));

    ::FindClose(hFind);
}

}
}
