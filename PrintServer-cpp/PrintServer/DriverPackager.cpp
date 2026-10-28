#include "pch.h"
#include "DriverPackager.h"
#include "Wmi.h"
#include "Json.h"
#include "Crypto.h"
#include "DriverSigning.h"
#include "LogHelper.h"

#include <shlwapi.h>
#include <algorithm>
#include <map>
#include <set>
#include <stdio.h>

#include "third_party/miniz/miniz.h"

#pragma comment(lib, "shlwapi.lib")

namespace ps
{

const char* const DriverPackager::kManifestFileName = "_printserver_driver.json";

namespace
{

// ===========================================================================
// 基础工具
// ===========================================================================

std::wstring TrimWideLocal(const std::wstring& text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t'))
        ++begin;
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t'))
        --end;
    return text.substr(begin, end - begin);
}

std::wstring ToLowerWide(const std::wstring& text)
{
    std::wstring result = text;
    for (wchar_t& ch : result)
        ch = (wchar_t)::towlower(ch);
    return result;
}

bool ContainsNoCase(const std::string& haystack, const std::string& needle)
{
    if (needle.empty())
        return false;
    return ToLowerAscii(haystack).find(ToLowerAscii(needle)) != std::string::npos;
}

std::wstring WindowsDirectory()
{
    wchar_t buf[MAX_PATH] = { 0 };
    ::GetWindowsDirectoryW(buf, MAX_PATH);
    return buf;
}

// 32 位进程运行在 64 位系统上时 System32 会被重定向到 SysWOW64；
// 打印驱动、DriverStore、CatRoot 都在真实的 System32 下，这里显式绕过重定向。
bool IsWow64On64BitOs()
{
    BOOL wow64 = FALSE;
    if (!::IsWow64Process(::GetCurrentProcess(), &wow64))
        return false;
    return wow64 == TRUE;
}

std::wstring System32Directory()
{
    if (IsWow64On64BitOs())
        return WindowsDirectory() + L"\\Sysnative";
    return WindowsDirectory() + L"\\System32";
}

bool PathExists(const std::wstring& path)
{
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path)
{
    DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty())
        return name;
    wchar_t last = dir[dir.size() - 1];
    if (last == L'\\' || last == L'/')
        return dir + name;
    return dir + L"\\" + name;
}

long long FileSize(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA attr = { 0 };
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
        return -1;
    LARGE_INTEGER size;
    size.HighPart = (LONG)attr.nFileSizeHigh;
    size.LowPart = attr.nFileSizeLow;
    return size.QuadPart;
}

// 枚举目录下匹配模式的文件（非递归）
void EnumFiles(const std::wstring& dir, const wchar_t* pattern, std::vector<std::wstring>& out)
{
    WIN32_FIND_DATAW fd = { 0 };
    std::wstring query = dir + L"\\" + pattern;
    HANDLE h = ::FindFirstFileW(query.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            out.push_back(dir + L"\\" + fd.cFileName);
    } while (::FindNextFileW(h, &fd));

    ::FindClose(h);
}

// 枚举子目录（非递归）
void EnumDirs(const std::wstring& dir, std::vector<std::wstring>& out)
{
    WIN32_FIND_DATAW fd = { 0 };
    HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0)
        {
            out.push_back(dir + L"\\" + fd.cFileName);
        }
    } while (::FindNextFileW(h, &fd));

    ::FindClose(h);
}

// 递归枚举文件
void EnumFilesRecursive(const std::wstring& dir, std::vector<std::wstring>& out)
{
    std::vector<std::wstring> files;
    EnumFiles(dir, L"*", files);
    for (const auto& f : files)
        out.push_back(f);

    std::vector<std::wstring> dirs;
    EnumDirs(dir, dirs);
    for (const auto& d : dirs)
        EnumFilesRecursive(d, out);
}

std::wstring GetFileName(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

// 取目录名（忽略结尾的分隔符）
std::wstring DirBaseName(const std::wstring& path)
{
    std::wstring trimmed = path;
    while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/'))
        trimmed.pop_back();
    return GetFileName(trimmed);
}

// 取父目录（忽略结尾的分隔符）；无父目录时返回空串
std::wstring ParentDirectory(const std::wstring& path)
{
    std::wstring trimmed = path;
    while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/'))
        trimmed.pop_back();

    size_t pos = trimmed.find_last_of(L"\\/");
    if (pos == std::wstring::npos || pos < 2)
        return std::wstring();
    return trimmed.substr(0, pos);
}

// WQL 字符串转义（反斜杠与单引号）
std::wstring EscapeWqlString(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size() + 8);
    for (wchar_t ch : text)
    {
        if (ch == L'\\' || ch == L'\'')
            out.push_back(L'\\');
        out.push_back(ch);
    }
    return out;
}

std::wstring GetExtensionLower(const std::wstring& path)
{
    std::wstring name = GetFileName(path);
    size_t pos = name.find_last_of(L'.');
    if (pos == std::wstring::npos)
        return L"";
    std::wstring ext = name.substr(pos);
    for (wchar_t& c : ext)
        c = (wchar_t)towlower(c);
    return ext;
}

// 读取文件全部内容
bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& out)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size = { 0 };
    if (!::GetFileSizeEx(h, &size) || size.QuadPart < 0)
    {
        ::CloseHandle(h);
        return false;
    }

    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    bool ok = true;
    if (!out.empty())
        ok = (::ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr) != 0);

    ::CloseHandle(h);
    out.resize(read);
    return ok;
}

// 递归删除目录（先删子目录，最后删自身）
void RemoveDirectoryTree(const std::wstring& dir)
{
    std::vector<std::wstring> subDirs;
    EnumDirs(dir, subDirs);
    for (const auto& sub : subDirs)
        RemoveDirectoryTree(sub);

    std::vector<std::wstring> files;
    EnumFiles(dir, L"*", files);
    for (const auto& file : files)
        ::DeleteFileW(file.c_str());

    ::RemoveDirectoryW(dir.c_str());
}

// 文件 SHA256（小写十六进制）；失败返回空串
std::string Sha256HexFile(const std::wstring& path)
{
    std::vector<unsigned char> data;
    if (!ReadFileBytes(path, data))
        return std::string();

    std::vector<unsigned char> digest = Crypto::Sha256(data);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (unsigned char b : digest)
    {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

// ===========================================================================
// 注册表读取
// ===========================================================================

bool RegReadString(HKEY root, const std::wstring& subKey, const wchar_t* valueName,
                   std::wstring& out)
{
    out.clear();

    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    wchar_t buf[2048] = { 0 };
    DWORD size = sizeof(buf);
    DWORD type = 0;
    LONG rc = ::RegQueryValueExW(key, valueName, nullptr, &type, (BYTE*)buf, &size);
    ::RegCloseKey(key);

    if (rc != ERROR_SUCCESS || type != REG_SZ)
        return false;

    out = TrimWideLocal(buf);
    return !out.empty();
}

// 读取 REG_MULTI_SZ；Active / Dependant Files 在不同 Windows 上类型不一致，
// 因此这里对 REG_SZ 也做兼容。
bool RegReadMultiString(HKEY root, const std::wstring& subKey, const wchar_t* valueName,
                        std::vector<std::wstring>& out)
{
    out.clear();

    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    DWORD size = 0;
    DWORD type = 0;
    if (::RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t))
    {
        ::RegCloseKey(key);
        return false;
    }

    if (type != REG_MULTI_SZ && type != REG_SZ)
    {
        ::RegCloseKey(key);
        return false;
    }

    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, L'\0');
    DWORD readSize = (DWORD)(buf.size() * sizeof(wchar_t));
    LONG rc = ::RegQueryValueExW(key, valueName, nullptr, &type, (BYTE*)buf.data(), &readSize);
    ::RegCloseKey(key);

    if (rc != ERROR_SUCCESS)
        return false;

    if (type == REG_SZ)
    {
        std::wstring single = TrimWideLocal(buf.data());
        if (!single.empty())
            out.push_back(single);
        return !out.empty();
    }

    for (const wchar_t* p = buf.data(); *p != L'\0'; p += wcslen(p) + 1)
    {
        std::wstring item = TrimWideLocal(p);
        if (!item.empty())
            out.push_back(item);
    }
    return !out.empty();
}

// ===========================================================================
// INF 解析
// ===========================================================================

struct InfInfo
{
    std::wstring catalogFile;
    std::vector<std::wstring> sourceDisksFiles;   // 仅文件名
    std::vector<std::wstring> modelNames;
    bool parsed = false;
};

struct InfSection
{
    std::wstring name;
    std::vector<std::wstring> lines;
};

std::wstring StripQuotes(std::wstring value)
{
    value = TrimWideLocal(value);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
        value = value.substr(1, value.size() - 2);
    return TrimWideLocal(value);
}

std::wstring ReadInfText(const std::wstring& path)
{
    std::vector<unsigned char> data;
    if (!ReadFileBytes(path, data) || data.empty())
        return std::wstring();

    // UTF-16LE（带 BOM）
    if (data.size() >= 2 && data[0] == 0xFF && data[1] == 0xFE)
    {
        size_t chars = (data.size() - 2) / 2;
        std::wstring text(chars, L'\0');
        if (chars > 0)
            memcpy(&text[0], data.data() + 2, chars * sizeof(wchar_t));
        return text;
    }

    // 其余按 ANSI 处理
    int need = ::MultiByteToWideChar(CP_ACP, 0, (const char*)data.data(), (int)data.size(), nullptr, 0);
    if (need <= 0)
        return std::wstring();

    std::wstring text((size_t)need, L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, (const char*)data.data(), (int)data.size(), &text[0], need);
    return text;
}

// 判断带架构后缀的节是否属于目标架构
bool SectionMatchesArch(const std::wstring& sectionLower, const std::wstring& archHintLower)
{
    static const wchar_t* kTokens[] = { L"ntamd64", L"ntx86", L"ntia64", L"ntarm64",
                                        L"amd64", L"x86", L"ia64", L"arm64" };

    for (const wchar_t* token : kTokens)
    {
        if (sectionLower.find(token) != std::wstring::npos)
        {
            if (archHintLower.empty())
                return false;   // 目标架构未知时不纳入，避免误报缺失
            return sectionLower.find(archHintLower) != std::wstring::npos;
        }
    }
    return true;    // 通用节
}

// 解析 INF：CatalogFile / [SourceDisksFiles] / 型号名
bool ParseInf(const std::wstring& path, const std::wstring& archHintLower, InfInfo& out)
{
    out = InfInfo();

    std::wstring text = ReadInfText(path);
    if (text.empty())
        return false;

    std::vector<InfSection> sections;
    {
        size_t pos = 0;
        InfSection* current = nullptr;

        while (pos <= text.size())
        {
            size_t end = text.find_first_of(L"\r\n", pos);
            std::wstring line = (end == std::wstring::npos)
                                    ? text.substr(pos)
                                    : text.substr(pos, end - pos);
            pos = (end == std::wstring::npos) ? (text.size() + 1) : (end + 1);

            std::wstring trimmed = TrimWideLocal(line);
            if (trimmed.empty() || trimmed.front() == L';')
                continue;

            if (trimmed.front() == L'[' && trimmed.back() == L']')
            {
                sections.push_back(InfSection{ trimmed.substr(1, trimmed.size() - 2), {} });
                current = &sections.back();
                continue;
            }

            if (current != nullptr)
                current->lines.push_back(trimmed);
        }
    }

    auto findSection = [&sections](const std::wstring& name) -> const InfSection* {
        std::wstring lower = ToLowerWide(name);
        for (const auto& section : sections)
        {
            if (ToLowerWide(section.name) == lower)
                return &section;
        }
        return nullptr;
    };

    // [Version] CatalogFile
    if (const InfSection* version = findSection(L"Version"))
    {
        for (const auto& line : version->lines)
        {
            if (ToLowerWide(line).rfind(L"catalogfile", 0) == 0)
            {
                size_t eq = line.find(L'=');
                if (eq != std::wstring::npos)
                    out.catalogFile = GetFileName(StripQuotes(line.substr(eq + 1)));
                break;
            }
        }
    }

    // [SourceDisksFiles*]
    for (const auto& section : sections)
    {
        std::wstring lower = ToLowerWide(section.name);
        if (lower.rfind(L"sourcedisksfiles", 0) != 0)
            continue;
        if (!SectionMatchesArch(lower, archHintLower))
            continue;

        for (const auto& line : section.lines)
        {
            size_t eq = line.find(L'=');
            std::wstring name = (eq == std::wstring::npos) ? line : line.substr(0, eq);
            name = GetFileName(StripQuotes(name));   // 去掉可能的子目录前缀
            if (!name.empty())
                out.sourceDisksFiles.push_back(name);
        }
    }

    // [Manufacturer] → 各型号节 → 型号名
    if (const InfSection* manufacturer = findSection(L"Manufacturer"))
    {
        std::vector<std::wstring> targets;

        for (const auto& line : manufacturer->lines)
        {
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos)
                continue;

            std::wstring rhs = line.substr(eq + 1);
            std::vector<std::wstring> parts;
            {
                std::wstring current;
                for (wchar_t ch : rhs)
                {
                    if (ch == L',')
                    {
                        parts.push_back(TrimWideLocal(current));
                        current.clear();
                    }
                    else
                    {
                        current.push_back(ch);
                    }
                }
                parts.push_back(TrimWideLocal(current));
            }

            if (parts.empty())
                continue;

            std::wstring base = StripQuotes(parts[0]);
            if (base.empty())
                continue;

            targets.push_back(base);
            for (size_t i = 1; i < parts.size(); ++i)
            {
                std::wstring arch = StripQuotes(parts[i]);
                if (!arch.empty())
                    targets.push_back(base + L"." + arch);
            }
        }

        std::set<std::wstring> seen;
        for (const auto& target : targets)
        {
            if (!seen.insert(ToLowerWide(target)).second)
                continue;

            const InfSection* section = findSection(target);
            if (section == nullptr)
                continue;

            for (const auto& line : section->lines)
            {
                if (line.empty() || line.front() != L'"')
                    continue;

                size_t eq = line.find(L'=');
                if (eq == std::wstring::npos || eq < 2)
                    continue;

                std::wstring model = StripQuotes(line.substr(0, eq));
                if (model.size() >= 3)
                    out.modelNames.push_back(model);
            }
        }
    }

    out.parsed = true;
    return true;
}

// ===========================================================================
// 驱动包定位
// ===========================================================================

struct ResolvedPackage
{
    bool ok = false;
    std::string driverName;
    std::string driverVersion;
    std::string driverDate;
    std::string manufacturer;

    std::wstring infName;        // oemXX.inf
    std::wstring infFileName;    // 包内 INF 文件名
    std::wstring arch;           // amd64 / x86
    std::wstring source;         // driverstore / registry
    std::wstring environment;    // Windows x64 / Windows NT x86
    std::wstring packageDir;     // 路线A：直接打包的目录（空则用 files 展平）
    std::vector<std::wstring> files;
    std::vector<std::wstring> missing;
    InfInfo inf;
};

// 在目录内查找打包用的 INF：优先根目录，其次递归
std::wstring FindInfInDir(const std::wstring& dir)
{
    std::vector<std::wstring> rootFiles;
    EnumFiles(dir, L"*.inf", rootFiles);

    std::wstring fallback;
    for (const auto& file : rootFiles)
    {
        std::wstring name = ToLowerWide(GetFileName(file));
        if (name == L"setup.inf")
            continue;
        return file;
    }

    std::vector<std::wstring> allFiles;
    EnumFilesRecursive(dir, allFiles);
    for (const auto& file : allFiles)
    {
        if (GetExtensionLower(file) != L".inf")
            continue;
        if (ToLowerWide(GetFileName(file)) == L"setup.inf")
            continue;
        if (fallback.empty())
            fallback = file;
    }
    return fallback;
}

std::wstring GuessArchFromPackageName(const std::wstring& packageName)
{
    std::wstring lower = ToLowerWide(packageName);
    if (lower.find(L"_amd64") != std::wstring::npos || lower.find(L"amd64") != std::wstring::npos)
        return L"amd64";
    if (lower.find(L"_x86") != std::wstring::npos)
        return L"x86";
    if (lower.find(L"arm64") != std::wstring::npos)
        return L"arm64";
    return std::wstring();
}

// 由 INF 名定位 DriverStore 包目录：
//   1) oemNN.inf → DriverDatabase\DriverInfFiles\oemNN.inf 的 Active（标准映射）
//   2) 原始 INF 名 → FileRepository 下以 "<原始名>.inf_" 开头的包目录
//      （实测 Win10 上 DriverInfFiles 可能不存在，此时靠这条反查）
bool ResolveDriverStoreByInfName(const std::wstring& infName, std::wstring& outDir)
{
    outDir.clear();
    if (infName.empty())
        return false;

    std::wstring shortName = GetFileName(infName);
    std::wstring repo = System32Directory() + L"\\DriverStore\\FileRepository";

    // 1) DriverDatabase 标准映射
    std::wstring key = L"SYSTEM\\CurrentControlSet\\Control\\DriverDatabase\\DriverInfFiles\\" + shortName;
    std::wstring active;
    if (!RegReadString(HKEY_LOCAL_MACHINE, key, L"Active", active))
    {
        // 部分系统上 Active 是 REG_MULTI_SZ，取最后一项（最新版）
        std::vector<std::wstring> actives;
        if (RegReadMultiString(HKEY_LOCAL_MACHINE, key, L"Active", actives) && !actives.empty())
            active = actives.back();
    }

    if (!active.empty())
    {
        std::wstring dir = JoinPath(repo, active);
        if (IsDirectory(dir))
        {
            outDir = dir;
            return true;
        }
    }

    // 2) 原始 INF 名反查：包目录名形如 "<原始INF名>.inf_<arch>_<hash>"
    if (IsDirectory(repo))
    {
        std::wstring prefix = ToLowerWide(shortName) + L".inf_";
        std::vector<std::wstring> packages;
        EnumDirs(repo, packages);
        for (const auto& dir : packages)
        {
            std::wstring base = ToLowerWide(GetFileName(dir));
            if (base.rfind(prefix, 0) == 0)
            {
                outDir = dir;
                return true;
            }
        }
    }

    return false;
}

// 查找 .cat（INF 声明的 CatalogFile）
std::wstring FindCatalogFile(const std::wstring& catalogName)
{
    if (catalogName.empty())
        return std::wstring();

    std::wstring file = GetFileName(catalogName);

    // 1) C:\Windows\INF
    std::wstring candidate = JoinPath(WindowsDirectory() + L"\\INF", file);
    if (PathExists(candidate))
        return candidate;

    // 2) System32\CatRoot\<GUID>\<file>（一层子目录）
    std::wstring catRoot = System32Directory() + L"\\CatRoot";
    std::vector<std::wstring> catRootDirs;
    EnumDirs(catRoot, catRootDirs);
    for (const auto& dir : catRootDirs)
    {
        candidate = JoinPath(dir, file);
        if (PathExists(candidate))
            return candidate;
    }

    // 3) DriverStore\FileRepository\<pkg>\<file>
    std::wstring repo = System32Directory() + L"\\DriverStore\\FileRepository";
    std::vector<std::wstring> packages;
    EnumDirs(repo, packages);
    for (const auto& pkg : packages)
    {
        candidate = JoinPath(pkg, file);
        if (PathExists(candidate))
            return candidate;
    }

    return std::wstring();
}

void AddUnique(std::vector<std::wstring>& list, std::set<std::wstring>& seen, const std::wstring& file)
{
    if (file.empty())
        return;

    std::wstring key = ToLowerWide(file);
    if (seen.insert(key).second)
        list.push_back(file);
}

// 判断是否是 Windows 自带的打印核心文件（厂商 INF 常引用，但本就不在厂商包里）
bool IsWindowsInboxPrinterFile(const std::wstring& nameLower)
{
    static const wchar_t* kInbox[] = {
        L"unidrv.dll", L"unidrvui.dll", L"unidrv.hlp",
        L"pscript5.dll", L"ps5ui.dll", L"pscript.hlp",
        L"stddtype.gdl", L"stdschmx.gdl", L"stdnames.gpd", L"ttfsub.gpd",
        L"msxpsinc.gpd", L"msxpsinc.ppd", L"p6disp.gpd",
        L"printconfig.dll", L"unishare.gpd", L"mxdwdrv.dll", L"mxdw.gpd",
        L"ntprint.inf",
    };

    for (const wchar_t* item : kInbox)
    {
        if (nameLower == item)
            return true;
    }

    // 系统自带 INI（prnms001.inf / prnms003.inf ...）也是整包引用
    if (nameLower.size() > 4 && nameLower.rfind(L"prnms", 0) == 0 &&
        nameLower.compare(nameLower.size() - 4, 4, L".inf") == 0)
    {
        return true;
    }

    return false;
}

// 按 INF 声明的文件清单核对包内是否齐全
void ComputeMissing(const std::vector<std::wstring>& files, const InfInfo& inf,
                    std::vector<std::wstring>& missing)
{
    missing.clear();

    std::set<std::wstring> present;
    for (const auto& file : files)
        present.insert(ToLowerWide(GetFileName(file)));

    std::set<std::wstring> declared;
    for (const auto& name : inf.sourceDisksFiles)
        declared.insert(ToLowerWide(GetFileName(name)));

    if (!inf.catalogFile.empty())
        declared.insert(ToLowerWide(GetFileName(inf.catalogFile)));

    for (const auto& name : declared)
    {
        if (IsWindowsInboxPrinterFile(name))
            continue;
        if (present.find(name) == present.end())
            missing.push_back(name);
    }
}

// 枚举驱动打印环境 / 版本，按精确子键名定位注册表项
bool FindPrintDriverRegistryKey(const std::string& driverNameUtf8,
                                std::wstring& regPath, std::wstring& environment,
                                std::wstring& version)
{
    struct EnvCandidate { const wchar_t* env; const wchar_t* archDir; };
    static const EnvCandidate kEnvs[] = {
        { L"Windows x64", L"x64" },
        { L"Windows NT x86", L"W32X86" },
    };
    static const wchar_t* kVersions[] = { L"Version-3", L"Version-4" };

    std::wstring driverName = Utf8ToWide(driverNameUtf8);

    for (const auto& env : kEnvs)
    {
        for (const wchar_t* ver : kVersions)
        {
            std::wstring path = L"SYSTEM\\CurrentControlSet\\Control\\Print\\Environments\\";
            path += env.env;
            path += L"\\Drivers\\";
            path += ver;
            path += L"\\";
            path += driverName;

            HKEY key = nullptr;
            if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS)
            {
                ::RegCloseKey(key);
                regPath = path;
                environment = env.env;
                version = ver;
                return true;
            }
        }
    }

    return false;
}

// 主定位流程
bool ResolveDriverPackage(const std::string& printerNameUtf8, ResolvedPackage& out)
{
    out = ResolvedPackage();
    out.driverName = printerNameUtf8;

    // ---- 1. 打印机 → 驱动名 / 驱动版本信息 ----
    {
        WmiQuery q;
        if (q.IsValid())
        {
            std::wstring wql = L"SELECT * FROM Win32_Printer WHERE Name = '" +
                               EscapeWqlString(Utf8ToWide(printerNameUtf8)) + L"'";
            q.ForEach(wql, [&](IWbemClassObject* obj) -> bool {
                std::wstring value;
                if (WmiQuery::GetString(obj, L"DriverName", value) && !value.empty())
                    out.driverName = WideToUtf8(value);
                return false;
            });
        }
    }

    std::wstring infName;
    std::wstring driverPath;

    // ---- 2. Win32_PrinterDriver：InfName / DriverPath / 版本信息 ----
    if (!out.driverName.empty())
    {
        WmiQuery q;
        if (q.IsValid())
        {
            // Win32_PrinterDriver.Name 的格式为 "驱动名,版本,环境"
            // （如 "Lenovo M7298W,3,Windows x64"），必须用前缀 LIKE 匹配
            std::wstring wql = L"SELECT * FROM Win32_PrinterDriver WHERE Name LIKE '" +
                               EscapeWqlString(Utf8ToWide(out.driverName)) + L",%'";
            q.ForEach(wql, [&](IWbemClassObject* obj) -> bool {
                std::wstring value;
                if (WmiQuery::GetString(obj, L"InfName", value) && !value.empty())
                    infName = value;
                if (WmiQuery::GetString(obj, L"DriverPath", value) && !value.empty())
                    driverPath = value;
                if (WmiQuery::GetString(obj, L"DriverVersion", value))
                    out.driverVersion = WideToUtf8(value);
                if (WmiQuery::GetString(obj, L"DriverDate", value))
                    out.driverDate = WideToUtf8(value);
                if (WmiQuery::GetString(obj, L"ManufacturerName", value))
                    out.manufacturer = WideToUtf8(value);
                return false;
            });
        }
    }

    // ---- 3. 注册表精确项（InfPath 是权威入口）----
    std::wstring regPath;
    std::wstring environment;
    std::wstring version;
    bool hasRegKey = FindPrintDriverRegistryKey(out.driverName, regPath, environment, version);

    // 实测：InfPath 给的是「DriverStore 包内 INF 的完整路径」，例如
    //   C:\Windows\System32\DriverStore\FileRepository\lnthrgz64.inf_amd64_<hash>\lnthrgz64.inf
    // 其所在目录就是自洽完整的驱动包（含 .cat）。
    // 少数系统上它只是 "oemNN.inf"（此时退回到反查）。
    std::wstring infPathFull;
    if (hasRegKey)
    {
        std::wstring value;
        if (RegReadString(HKEY_LOCAL_MACHINE, regPath, L"InfPath", value) && !value.empty())
            infPathFull = value;
    }

    if (!infPathFull.empty())
        infName = GetFileName(infPathFull);

    std::wstring arch;
    if (hasRegKey)
        arch = (environment == L"Windows x64") ? L"amd64" : L"x86";

    std::wstring archHint = arch;
    if (archHint.empty() && !driverPath.empty())
        archHint = GuessArchFromPackageName(DirBaseName(driverPath));

    // ---- 4. 路线A：DriverStore 整包 ----
    std::wstring storeDir;

    // 4.1 InfPath 的父目录即 DriverStore 包（最可靠，实测命中）
    if (!infPathFull.empty() && PathExists(infPathFull))
    {
        std::wstring dir = ParentDirectory(infPathFull);
        if (!dir.empty() && IsDirectory(dir) && ContainsNoCase(WideToUtf8(dir), "driverstore"))
            storeDir = dir;
    }

    // 4.2 Win32_PrinterDriver.DriverPath 指向 DriverStore（v4 驱动常见，
    //     形如 ...\FileRepository\<pkg>\Amd64\xxx.dll）
    if (storeDir.empty() && !driverPath.empty())
    {
        std::wstring dir = IsDirectory(driverPath) ? driverPath : ParentDirectory(driverPath);
        if (!dir.empty() && IsDirectory(dir) && ContainsNoCase(WideToUtf8(dir), "driverstore"))
        {
            std::wstring lowerDir = ToLowerWide(dir);
            std::wstring repoTag = L"\\filerepository\\";
            size_t pos = lowerDir.find(repoTag);
            if (pos != std::wstring::npos)
            {
                size_t nameStart = pos + repoTag.size();
                size_t nextSep = dir.find(L'\\', nameStart);
                if (nextSep != std::wstring::npos)
                    dir = dir.substr(0, nextSep);
            }
            storeDir = dir;
        }
    }

    // 4.3 由 INF 名在 FileRepository 反查包目录
    if (storeDir.empty() && !infName.empty())
    {
        std::wstring resolved;
        if (ResolveDriverStoreByInfName(infName, resolved))
            storeDir = resolved;
    }

    // 路径可能带结尾反斜杠，统一去掉后再使用
    while (!storeDir.empty() && (storeDir.back() == L'\\' || storeDir.back() == L'/'))
        storeDir.pop_back();

    // 驱动包目录里应有 INF，否则不作为整包使用
    if (!storeDir.empty() && FindInfInDir(storeDir).empty())
        storeDir.clear();

    if (!storeDir.empty())
    {
        std::wstring infFile = FindInfInDir(storeDir);

        out.ok = true;
        out.source = L"driverstore";
        out.packageDir = storeDir;
        out.infName = infName;
        out.infFileName = GetFileName(infFile);
        out.environment = environment;
        out.arch = archHint.empty() ? GuessArchFromPackageName(DirBaseName(storeDir)) : archHint;

        EnumFilesRecursive(storeDir, out.files);

        std::wstring archHintForInf = (out.arch == L"amd64") ? L"amd64"
                                     : (out.arch == L"x86") ? L"x86" : L"";
        ParseInf(infFile, archHintForInf, out.inf);
        ComputeMissing(out.files, out.inf, out.missing);

        Log::WriteLog("[DriverPackager] 驱动包定位成功(DriverStore): " + WideToUtf8(storeDir) +
                      ", 文件数=" + std::to_string(out.files.size()) +
                      ", 缺失=" + std::to_string(out.missing.size()));
        return true;
    }

    // ---- 5. 路线B：注册表精确项 + spool 目录 ----
    if (hasRegKey)
    {
        std::wstring archDir = (environment == L"Windows x64") ? L"x64" : L"W32X86";
        std::wstring spoolDir = System32Directory() + L"\\spool\\drivers\\" + archDir;
        std::wstring spoolVerDir = JoinPath(spoolDir, version.substr(version.find(L'-') + 1));

        std::vector<std::wstring> files;
        std::set<std::wstring> seen;

        // INF 本体：优先用注册表 InfPath 指到的实际文件，其次按名去 C:\Windows\INF 找
        std::wstring infFile;
        if (!infPathFull.empty() && PathExists(infPathFull))
        {
            infFile = infPathFull;
            AddUnique(files, seen, infFile);
        }
        else if (!infName.empty())
        {
            std::wstring candidate = JoinPath(WindowsDirectory() + L"\\INF", GetFileName(infName));
            if (PathExists(candidate))
            {
                infFile = candidate;
                AddUnique(files, seen, candidate);
            }
        }

        // 驱动注册表值指向的文件
        // 注册表里驱动主 DLL 的值名实为 "Driver"（部分版本写作 "Driver File"）
        const wchar_t* kValueNames[] = { L"Configuration File", L"Data File",
                                         L"Driver", L"Driver File",
                                         L"Help File", L"Print Processor", L"Monitor" };
        for (const wchar_t* valueName : kValueNames)
        {
            std::wstring value;
            if (RegReadString(HKEY_LOCAL_MACHINE, regPath, valueName, value) && !value.empty())
            {
                std::wstring full = JoinPath(spoolVerDir, GetFileName(value));
                if (PathExists(full))
                    AddUnique(files, seen, full);
            }
        }

        // Dependant Files（REG_MULTI_SZ / REG_SZ 均兼容）
        std::vector<std::wstring> dependants;
        if (!RegReadMultiString(HKEY_LOCAL_MACHINE, regPath, L"Dependant Files", dependants))
        {
            if (!RegReadMultiString(HKEY_LOCAL_MACHINE, regPath, L"DependantFiles", dependants))
                RegReadMultiString(HKEY_LOCAL_MACHINE, regPath, L"Dependent Files", dependants);
        }
        for (const auto& item : dependants)
        {
            std::wstring full = JoinPath(spoolVerDir, GetFileName(item));
            if (PathExists(full))
                AddUnique(files, seen, full);
        }

        // INF 声明的 SourceDisksFiles
        InfInfo inf;
        if (!infFile.empty())
        {
            std::wstring archHintForInf = (environment == L"Windows x64") ? L"amd64" : L"x86";
            ParseInf(infFile, archHintForInf, inf);

            for (const auto& name : inf.sourceDisksFiles)
            {
                std::wstring full = JoinPath(spoolVerDir, name);
                if (PathExists(full))
                    AddUnique(files, seen, full);
            }
        }

        // .cat
        if (!inf.catalogFile.empty())
        {
            std::wstring cat = FindCatalogFile(inf.catalogFile);
            if (!cat.empty())
                AddUnique(files, seen, cat);
        }

        if (!infFile.empty() && !files.empty())
        {
            out.ok = true;
            out.source = L"registry";
            out.infName = infName;
            out.infFileName = GetFileName(infFile);
            out.environment = environment;
            out.arch = (environment == L"Windows x64") ? L"amd64" : L"x86";
            out.files = files;
            out.inf = inf;
            ComputeMissing(out.files, out.inf, out.missing);

            Log::WriteLog("[DriverPackager] 驱动包定位成功(注册表): " + WideToUtf8(regPath) +
                          ", 文件数=" + std::to_string(out.files.size()) +
                          ", 缺失=" + std::to_string(out.missing.size()));
            return true;
        }
    }

    Log::WriteLog("[DriverPackager] 未能定位驱动包: printer=" + printerNameUtf8 +
                  ", driver=" + out.driverName +
                  ", inf=" + WideToUtf8(infName) +
                  ", driverPath=" + WideToUtf8(driverPath));
    return false;
}

// ===========================================================================
// 清单 / ZIP
// ===========================================================================

struct ManifestEntry
{
    std::string relPath;
    long long size = 0;
    std::string sha256;
};

std::string BuildManifestJson(const ResolvedPackage& pkg, const std::string& printerNameUtf8,
                              const std::vector<ManifestEntry>& entries,
                              const std::wstring& infRelPath)
{
    JsonValue root = JsonValue::Object();
    root.Set("schema", JsonValue::Int(1));
    root.Set("printerName", JsonValue::Str(printerNameUtf8));
    root.Set("driverName", JsonValue::Str(pkg.driverName));
    root.Set("inf", JsonValue::Str(WideToUtf8(infRelPath)));
    root.Set("architecture", JsonValue::Str(WideToUtf8(pkg.arch)));
    root.Set("source", JsonValue::Str(WideToUtf8(pkg.source)));
    root.Set("catalogFile", JsonValue::Str(WideToUtf8(pkg.inf.catalogFile)));
    root.Set("complete", JsonValue::Bool(pkg.missing.empty() && !pkg.infFileName.empty()));

    JsonValue missing = JsonValue::Array();
    for (const auto& name : pkg.missing)
        missing.arr.push_back(JsonValue::Str(WideToUtf8(name)));
    root.Set("missing", missing);

    JsonValue models = JsonValue::Array();
    for (const auto& name : pkg.inf.modelNames)
        models.arr.push_back(JsonValue::Str(WideToUtf8(name)));
    root.Set("infModels", models);

    JsonValue files = JsonValue::Array();
    std::vector<std::string> paths;
    std::vector<std::string> hashes;
    paths.reserve(entries.size());
    hashes.reserve(entries.size());

    for (const auto& entry : entries)
    {
        JsonValue item = JsonValue::Object();
        item.Set("path", JsonValue::Str(entry.relPath));
        item.Set("size", JsonValue::Int(entry.size));
        item.Set("sha256", JsonValue::Str(entry.sha256));
        files.arr.push_back(item);

        paths.push_back(entry.relPath);
        hashes.push_back(entry.sha256);
    }
    root.Set("files", files);

    // 数字签名：把清单里的关键字段与全部文件哈希一起签，客户端用内置公钥验签。
    // 明文 HTTP 下只靠 SHA256 挡不住中间人（改包的人也能改清单），必须非对称签名。
    std::string payload = DriverSigning::BuildPayload(pkg.driverName,
                                                      WideToUtf8(infRelPath),
                                                      WideToUtf8(pkg.arch),
                                                      WideToUtf8(pkg.inf.catalogFile),
                                                      paths, hashes);
    std::string signature = DriverSigning::Sign(payload);
    root.Set("signature", JsonValue::Str(signature));

    if (signature.empty())
        Log::WriteLog("[DriverPackager] 严重: 清单签名失败，客户端将拒绝该驱动包");
    else
        Log::WriteLog("[DriverPackager] 清单已签名 (Base64 " +
                      std::to_string(signature.size()) + " 字节)");

    return root.Serialize();
}

std::wstring NormalizeRelative(const std::wstring& raw)
{
    std::wstring relative = raw;
    while (!relative.empty() && (relative[0] == L'\\' || relative[0] == L'/'))
        relative.erase(relative.begin());
    for (wchar_t& ch : relative)
    {
        if (ch == L'\\')
            ch = L'/';
    }
    return relative;
}

std::vector<unsigned char> ZipDirectoryInternal(
    const std::wstring& dirPath,
    const std::vector<std::pair<std::string, std::string>>& extraTextEntries)
{
    std::vector<unsigned char> result;

    std::vector<std::wstring> allFiles;
    EnumFilesRecursive(dirPath, allFiles);

    if (allFiles.empty() && extraTextEntries.empty())
        return result;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    if (!mz_zip_writer_init_heap(&zip, 0, 0))
        return result;

    size_t baseLen = dirPath.size();

    for (const auto& file : allFiles)
    {
        std::wstring relative = (file.size() > baseLen) ? file.substr(baseLen) : file;
        std::string entryName = WideToUtf8(NormalizeRelative(relative));

        // 自行读取内容后写入，避免 miniz 内部用 ANSI fopen 打开中文路径
        std::vector<unsigned char> content;
        if (!ReadFileBytes(file, content))
            continue;

        const void* data = content.empty() ? (const void*)"" : (const void*)content.data();
        mz_zip_writer_add_mem(&zip, entryName.c_str(), data, content.size(),
                              MZ_DEFAULT_COMPRESSION);
    }

    for (const auto& extra : extraTextEntries)
    {
        const void* data = extra.second.empty() ? (const void*)"" : (const void*)extra.second.data();
        mz_zip_writer_add_mem(&zip, extra.first.c_str(), data, extra.second.size(),
                              MZ_DEFAULT_COMPRESSION);
    }

    void* buffer = nullptr;
    size_t size = 0;

    if (mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size) && buffer != nullptr && size > 0)
        result.assign((unsigned char*)buffer, (unsigned char*)buffer + size);

    mz_zip_writer_end(&zip);
    if (buffer != nullptr)
        mz_free(buffer);

    return result;
}

std::wstring MakeTempDir()
{
    wchar_t tempPath[MAX_PATH] = { 0 };
    ::GetTempPathW(MAX_PATH, tempPath);

    std::wstring dir = std::wstring(tempPath) + L"PrintServer_Driver_" +
                       std::to_wstring(::GetTickCount64());
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

} // namespace

// ===========================================================================
// 对外接口
// ===========================================================================

std::vector<unsigned char> DriverPackager::ZipDirectory(const std::wstring& dirPath)
{
    return ZipDirectoryInternal(dirPath, std::vector<std::pair<std::string, std::string>>());
}

PrinterDriverInfo DriverPackager::GetPrinterDriverInfo(const std::string& printerNameUtf8)
{
    PrinterDriverInfo info;
    info.driverName = printerNameUtf8;

    ResolvedPackage pkg;
    if (ResolveDriverPackage(printerNameUtf8, pkg))
    {
        info.driverName = pkg.driverName;
        info.driverVersion = pkg.driverVersion;
        info.driverDate = pkg.driverDate;
        info.manufacturer = pkg.manufacturer;
        info.infPath = WideToUtf8(pkg.infName);
        info.driverStorePath = WideToUtf8(pkg.packageDir);
        info.architecture = WideToUtf8(pkg.arch);
        info.packageSource = WideToUtf8(pkg.source);
        info.driverFiles = pkg.files;
        info.driverFileCount = (int)pkg.files.size();
        info.missingFileCount = (int)pkg.missing.size();
        info.complete = pkg.missing.empty() && !pkg.infFileName.empty();
        info.hasDriverFiles = info.driverFileCount > 0;

        long long total = 0;
        for (const auto& file : pkg.files)
        {
            long long size = FileSize(file);
            if (size > 0)
                total += size;
        }
        info.driverPackageSize = total;
    }

    return info;
}

std::vector<unsigned char> DriverPackager::PackageDriverFiles(const std::string& printerNameUtf8)
{
    std::vector<unsigned char> result;
    std::wstring tempDir;

    try
    {
        ResolvedPackage pkg;
        if (!ResolveDriverPackage(printerNameUtf8, pkg))
            return result;

        std::vector<ManifestEntry> entries;
        std::wstring zipDir;
        std::wstring infRelPath;

        if (!pkg.packageDir.empty())
        {
            // 路线A：整包目录，直接打包（保留目录结构）
            zipDir = pkg.packageDir;
            size_t baseLen = zipDir.size();

            for (const auto& file : pkg.files)
            {
                std::wstring relative = (file.size() > baseLen) ? file.substr(baseLen) : GetFileName(file);
                std::wstring normalized = NormalizeRelative(relative);

                ManifestEntry entry;
                entry.relPath = WideToUtf8(normalized);
                entry.size = FileSize(file);
                entry.sha256 = Sha256HexFile(file);
                entries.push_back(entry);

                if (GetFileName(file) == pkg.infFileName)
                    infRelPath = normalized;
            }
        }
        else
        {
            // 路线B：展平复制到临时目录后打包（spool 目录本身是平的）
            tempDir = MakeTempDir();
            for (const auto& file : pkg.files)
            {
                std::wstring dest = JoinPath(tempDir, GetFileName(file));
                if (::CopyFileW(file.c_str(), dest.c_str(), FALSE))
                {
                    ManifestEntry entry;
                    entry.relPath = WideToUtf8(GetFileName(file));
                    entry.size = FileSize(dest);
                    entry.sha256 = Sha256HexFile(dest);
                    entries.push_back(entry);

                    if (GetFileName(file) == pkg.infFileName)
                        infRelPath = GetFileName(file);
                }
            }

            if (entries.empty())
            {
                if (!tempDir.empty())
                    RemoveDirectoryTree(tempDir);
                Log::WriteLog("[DriverPackager] 打包失败: 复制驱动文件为空");
                return result;
            }

            zipDir = tempDir;
        }

        std::string manifest = BuildManifestJson(pkg, printerNameUtf8, entries, infRelPath);

        std::vector<std::pair<std::string, std::string>> extras;
        extras.push_back(std::make_pair(std::string(kManifestFileName), manifest));

        result = ZipDirectoryInternal(zipDir, extras);

        if (!tempDir.empty())
            RemoveDirectoryTree(tempDir);

        Log::WriteLog("[DriverPackager] 打包完成: printer=" + printerNameUtf8 +
                      ", source=" + WideToUtf8(pkg.source) +
                      ", inf=" + WideToUtf8(infRelPath) +
                      ", 文件数=" + std::to_string(entries.size()) +
                      ", 缺失=" + std::to_string(pkg.missing.size()) +
                      ", zipBytes=" + std::to_string(result.size()));

        if (!pkg.missing.empty())
        {
            std::string joined;
            for (const auto& name : pkg.missing)
            {
                if (!joined.empty())
                    joined += ", ";
                joined += WideToUtf8(name);
            }
            Log::WriteLog("[DriverPackager] 警告: 驱动包缺少 INF 声明的文件: " + joined);
        }

        return result;
    }
    catch (const std::exception& ex)
    {
        Log::WriteLog("[DriverPackager] 打包异常: " + std::string(ex.what()));
        if (!tempDir.empty())
            RemoveDirectoryTree(tempDir);
        return result;
    }
    catch (...)
    {
        Log::WriteLog("[DriverPackager] 打包异常: 未知错误");
        if (!tempDir.empty())
            RemoveDirectoryTree(tempDir);
        return result;
    }
}

} // namespace ps
