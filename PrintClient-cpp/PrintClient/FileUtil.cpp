#include "pch.h"
#include "FileUtil.h"

#include <string>
#include <vector>
#include <algorithm>

#include "Wmi.h"    // ps::WideToUtf8 / Utf8ToWide

namespace ps
{

namespace
{

const wchar_t* kPathSeparators = L"\\/";

bool HasTrailingSeparator(const std::wstring& path)
{
    if (path.empty())
        return false;
    return path.find_last_of(kPathSeparators) == path.size() - 1;
}

void EnumerateFilesInto(const std::wstring& directory, const std::wstring& pattern,
                        std::vector<std::wstring>& files)
{
    std::wstring search = directory;
    if (!HasTrailingSeparator(search))
        search += L"\\";
    search += pattern;

    WIN32_FIND_DATAW data;
    memset(&data, 0, sizeof(data));

    HANDLE handle = ::FindFirstFileW(search.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            std::wstring path = directory;
            if (!HasTrailingSeparator(path))
                path += L"\\";
            path += data.cFileName;
            files.push_back(path);
        }
    } while (::FindNextFileW(handle, &data));

    ::FindClose(handle);
}

} // namespace

std::wstring PathCombine(const std::wstring& left, const std::wstring& right)
{
    if (left.empty())
        return right;
    if (right.empty())
        return left;

    std::wstring result = left;
    if (!HasTrailingSeparator(result))
        result += L"\\";

    result += right;
    return result;
}

std::wstring GetTempDirectory()
{
    wchar_t buffer[MAX_PATH + 2] = { 0 };
    DWORD length = ::GetTempPathW(_countof(buffer), buffer);
    return std::wstring(buffer, length);
}

std::wstring GetFileName(const std::wstring& path)
{
    size_t index = path.find_last_of(kPathSeparators);
    if (index == std::wstring::npos)
        return path;
    return path.substr(index + 1);
}

bool CreateDirectoryTree(const std::wstring& path)
{
    if (path.empty())
        return false;

    if (DirectoryExists(path))
        return true;

    std::wstring current;
    size_t start = 0;

    // 跳过 "C:" 形式的盘符前缀
    if (path.size() >= 2 && path[1] == L':')
    {
        current = path.substr(0, 2);
        start = 2;
    }

    while (start <= path.size())
    {
        size_t index = path.find_first_of(kPathSeparators, start);
        std::wstring part = (index == std::wstring::npos)
                                ? path.substr(start)
                                : path.substr(start, index - start);

        if (!part.empty())
        {
            if (current.empty())
                current = part;
            else if (HasTrailingSeparator(current))
                current += part;
            else
                current += L"\\" + part;

            ::CreateDirectoryW(current.c_str(), nullptr);
        }

        if (index == std::wstring::npos)
            break;

        start = index + 1;
    }

    return DirectoryExists(path);
}

bool EnsureDirectory(const std::wstring& path)
{
    return CreateDirectoryTree(path);
}

bool DirectoryExists(const std::wstring& path)
{
    DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool FileExists(const std::wstring& path)
{
    DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool RemoveDirectoryTree(const std::wstring& path)
{
    if (!DirectoryExists(path))
        return true;

    std::vector<std::wstring> files;
    EnumerateFilesRecursive(path, L"*", files);
    for (const std::wstring& file : files)
        ::DeleteFileW(file.c_str());

    // 自底向上删除空目录
    std::vector<std::wstring> directories;

    // 先收集全部子目录
    std::vector<std::wstring> pending;
    pending.push_back(path);
    while (!pending.empty())
    {
        std::wstring current = pending.back();
        pending.pop_back();
        directories.push_back(current);

        std::wstring pattern = current;
        if (!HasTrailingSeparator(pattern))
            pattern += L"\\";
        pattern += L"*";

        WIN32_FIND_DATAW data;
        memset(&data, 0, sizeof(data));
        HANDLE handle = ::FindFirstFileW(pattern.c_str(), &data);
        if (handle == INVALID_HANDLE_VALUE)
            continue;

        do
        {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                continue;
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
                continue;

            pending.push_back(PathCombine(current, data.cFileName));
        } while (::FindNextFileW(handle, &data));

        ::FindClose(handle);
    }

    for (size_t i = directories.size(); i > 0; --i)
    {
        ::SetFileAttributesW(directories[i - 1].c_str(), FILE_ATTRIBUTE_NORMAL);
        ::RemoveDirectoryW(directories[i - 1].c_str());
    }

    return !DirectoryExists(path);
}

bool CopyFileOverwrite(const std::wstring& source, const std::wstring& dest)
{
    return ::CopyFileW(source.c_str(), dest.c_str(), FALSE) != FALSE;
}

bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& bytes)
{
    bytes.clear();

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size;
    memset(&size, 0, sizeof(size));
    if (!::GetFileSizeEx(file, &size))
    {
        ::CloseHandle(file);
        return false;
    }

    if (size.QuadPart > 0)
    {
        bytes.resize((size_t)size.QuadPart);

        size_t offset = 0;
        while (offset < bytes.size())
        {
            DWORD read = 0;
            DWORD want = (DWORD)(std::min)((size_t)0x1000000, bytes.size() - offset);
            if (!::ReadFile(file, bytes.data() + offset, want, &read, nullptr) || read == 0)
            {
                ::CloseHandle(file);
                bytes.clear();
                return false;
            }

            offset += read;
        }
    }

    ::CloseHandle(file);
    return true;
}

bool WriteFileBytes(const std::wstring& path, const std::vector<unsigned char>& bytes)
{
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    size_t offset = 0;
    while (offset < bytes.size())
    {
        DWORD written = 0;
        DWORD want = (DWORD)(std::min)((size_t)0x1000000, bytes.size() - offset);
        if (!::WriteFile(file, bytes.data() + offset, want, &written, nullptr) || written == 0)
        {
            ::CloseHandle(file);
            return false;
        }

        offset += written;
    }

    ::CloseHandle(file);
    return true;
}

bool WriteFileTextUtf8(const std::wstring& path, const std::string& utf8Text)
{
    std::vector<unsigned char> bytes(utf8Text.begin(), utf8Text.end());
    return WriteFileBytes(path, bytes);
}

bool WriteFileTextUtf8Bom(const std::wstring& path, const std::string& utf8Text)
{
    std::vector<unsigned char> bytes;
    bytes.push_back(0xEF);
    bytes.push_back(0xBB);
    bytes.push_back(0xBF);
    bytes.insert(bytes.end(), utf8Text.begin(), utf8Text.end());
    return WriteFileBytes(path, bytes);
}

long long GetFileSize(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    memset(&data, 0, sizeof(data));
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return -1;

    return ((long long)data.nFileSizeHigh << 32) | (long long)data.nFileSizeLow;
}

void EnumerateFilesRecursive(const std::wstring& directory, const std::wstring& pattern,
                             std::vector<std::wstring>& files)
{
    EnumerateFilesInto(directory, pattern, files);

    std::wstring search = directory;
    if (!HasTrailingSeparator(search))
        search += L"\\";
    search += L"*";

    WIN32_FIND_DATAW data;
    memset(&data, 0, sizeof(data));
    HANDLE handle = ::FindFirstFileW(search.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            continue;
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
            continue;

        EnumerateFilesRecursive(PathCombine(directory, data.cFileName), pattern, files);
    } while (::FindNextFileW(handle, &data));

    ::FindClose(handle);
}

std::string SanitizeFileName(const std::string& name)
{
    std::wstring wide = Utf8ToWide(name);
    return WideToUtf8(SanitizeFileNameW(wide));
}

std::wstring SanitizeFileNameW(const std::wstring& name)
{
    std::wstring result = name;
    for (wchar_t& ch : result)
    {
        if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch) != nullptr)
            ch = L'_';
    }
    return result;
}

std::string FormatSize(long long bytes)
{
    const char* sizes[] = { "B", "KB", "MB", "GB" };
    int order = 0;
    double size = (double)bytes;

    while (size >= 1024.0 && order < 3)
    {
        ++order;
        size /= 1024.0;
    }

    char number[64] = { 0 };
    _snprintf_s(number, sizeof(number), _TRUNCATE, "%.2f", size);

    // 对应 "{0:0.##}"：去掉多余的 0 和小数点
    std::string text = number;
    if (text.find('.') != std::string::npos)
    {
        while (!text.empty() && text.back() == '0')
            text.pop_back();
        if (!text.empty() && text.back() == '.')
            text.pop_back();
    }

    return text + " " + sizes[order];
}

std::string TrimAscii(const std::string& text)
{
    size_t begin = 0;
    while (begin < text.size() && (unsigned char)text[begin] <= ' ')
        ++begin;

    size_t end = text.size();
    while (end > begin && (unsigned char)text[end - 1] <= ' ')
        --end;

    return text.substr(begin, end - begin);
}

std::wstring TrimWide(const std::wstring& text)
{
    size_t begin = 0;
    while (begin < text.size() && iswspace(text[begin]))
        ++begin;

    size_t end = text.size();
    while (end > begin && iswspace(text[end - 1]))
        --end;

    return text.substr(begin, end - begin);
}

bool EqualsIgnoreCaseAscii(const std::string& text, const char* value)
{
    return _stricmp(text.c_str(), value) == 0;
}

bool StartsWithIgnoreCaseAscii(const std::string& text, const char* prefix)
{
    size_t length = strlen(prefix);
    if (text.size() < length)
        return false;

    return _strnicmp(text.c_str(), prefix, length) == 0;
}

std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string current;

    for (size_t i = 0; i < text.size(); ++i)
    {
        char ch = text[i];

        if (ch == '\r')
        {
            lines.push_back(current);
            current.clear();
            if (i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
        }
        else if (ch == '\n')
        {
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

    return lines;
}

std::string AnsiToUtf8(const std::vector<char>& bytes)
{
    if (bytes.empty())
        return std::string();

    int wideLength = ::MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (wideLength <= 0)
        return std::string();

    std::wstring wide((size_t)wideLength, L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), &wide[0], wideLength);

    return WideToUtf8(wide);
}

} // namespace ps
