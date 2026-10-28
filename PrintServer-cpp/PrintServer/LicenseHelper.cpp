#include "pch.h"
#include "LicenseHelper.h"
#include "AntiDebug.h"
#include "Crypto.h"
#include "MachineCode.h"
#include "LogHelper.h"
#include "Config.h"
#include "Json.h"
#include "Wmi.h"
#include "BuildConfig.h"
#include "TlsTrust.h"

#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <time.h>

#pragma comment(lib, "winhttp.lib")

namespace ps
{
namespace License
{

// ===========================================================================
// 全局状态（对应 C# 的静态字段）
// ===========================================================================

namespace
{

std::mutex g_mutex;
Status g_currentStatus = Status::Unknown;
std::string g_currentLicenseKey;
std::string g_machineCode;
std::string g_expiresAt;
std::string g_serverBaseUrl;

std::atomic<bool> g_isChecking(false);
std::atomic<bool> g_timerRunning(false);
std::thread g_timerThread;
std::mutex g_timerMutex;
std::condition_variable g_timerCv;

StatusChangedHandler g_statusChangedHandler;

const char* kLicenseFileName = "license.dat";
const char* kPendingReportFileName = ".pending_report";

std::wstring ExeDirectory()
{
    return Config::GetExeDirectory();
}

std::wstring JoinPath(const std::wstring& dir, const char* name)
{
    return dir + L"\\" + Utf8ToWide(name);
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

// 对应 C# 的 !string.IsNullOrEmpty(x) && x != "null" && x != ""
bool HasExpiryValue(const std::string& value)
{
    return !value.empty() && value != "null";
}

// 解析服务端返回的过期时间（对应 C# DateTime.TryParse 的常见格式）
bool TryParseExpiry(const std::string& text, struct tm& out)
{
    if (text.empty())
        return false;

    memset(&out, 0, sizeof(out));
    out.tm_isdst = -1;

    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;

    // "yyyy-MM-dd HH:mm:ss" 或 "yyyy-MM-ddTHH:mm:ss"
    if (sscanf_s(text.c_str(), "%4d-%2d-%2d%*c%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) == 6)
    {
        if (y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31)
            return false;
        out.tm_year = y - 1900;
        out.tm_mon = mo - 1;
        out.tm_mday = d;
        out.tm_hour = (h >= 0 && h <= 23) ? h : 0;
        out.tm_min = (mi >= 0 && mi <= 59) ? mi : 0;
        out.tm_sec = (s >= 0 && s <= 60) ? s : 0;
        return true;
    }

    // 仅日期 "yyyy-MM-dd"
    if (sscanf_s(text.c_str(), "%4d-%2d-%2d", &y, &mo, &d) == 3)
    {
        if (y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31)
            return false;
        out.tm_year = y - 1900;
        out.tm_mon = mo - 1;
        out.tm_mday = d;
        return true;
    }

    return false;
}

bool ExpiryIsPast(const std::string& text)
{
    struct tm tmv;
    if (!TryParseExpiry(text, tmv))
        return false; // 解析失败视为未过期（与 C# TryParse 失败时不处理一致）
    time_t expire = mktime(&tmv);
    return (expire != (time_t)-1 && expire < time(nullptr));
}

std::string FormatExpiryText(const std::string& text)
{
    struct tm tmv;
    if (!TryParseExpiry(text, tmv))
        return text;

    char buf[64] = { 0 };
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%04d-%02d-%02d %02d:%02d",
                tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    return buf;
}

std::string CurrentTimestamp()
{
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char buf[32] = { 0 };
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// 读取本地文件为 UTF-8 文本（用于 license.dat / .pending_report 的文本形式）
std::string ReadTextFileRaw(const std::wstring& path)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
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

    return std::string(buf.begin(), buf.end());
}

bool WriteTextFileRaw(const std::wstring& path, const std::string& text)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    bool ok = text.empty() ||
              (::WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr) != 0);
    ::CloseHandle(h);
    return ok;
}

} // namespace

// ===========================================================================
// 内部辅助（供 AntiDebug 等调用）
// ===========================================================================

namespace Internal
{

std::wstring LicenseFilePath()
{
    return JoinPath(ExeDirectory(), kLicenseFileName);
}

std::wstring PendingReportPath()
{
    return JoinPath(ExeDirectory(), kPendingReportFileName);
}

std::string GetDefaultSecretKey()
{
    // 对应 C#：读程序集特性，失败时用兜底密钥
    if (BuildConfig::SecretKey != nullptr && BuildConfig::SecretKey[0] != '\0')
        return BuildConfig::SecretKey;
    return BuildConfig::SecretKeyFallback;
}

int GetLicenseValidationIntervalHours()
{
    int hours = BuildConfig::LicenseValidationIntervalHours;
    return (hours > 0) ? hours : 3;
}

std::string BuildJson(const std::vector<std::pair<std::string, std::string>>& pairs)
{
    JsonValue obj = JsonValue::Object();
    for (const auto& kv : pairs)
        obj.Set(kv.first, JsonValue::Str(kv.second));
    return obj.Serialize();
}

// 解析 URL 的主机名 / 端口 / 是否 HTTPS
bool CrackTarget(const std::string& url, std::wstring& host, INTERNET_PORT& port, bool& https)
{
    std::wstring wurl = Utf8ToWide(url);

    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);

    wchar_t hostBuf[256] = { 0 };
    wchar_t pathBuf[2048] = { 0 };
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = _countof(hostBuf);
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = _countof(pathBuf);

    if (!::WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
        return false;

    host.assign(hostBuf, uc.dwHostNameLength);
    port = uc.nPort;
    https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

// 单次 WinHTTP POST 尝试
// relaxUnknownCa：是否忽略"未知颁发机构"错误。仅当 TlsTrust 已确认服务器证书
// 确实由内嵌根证书签发、且主机名/有效期/用途全部合法时才允许。
bool WinHttpPostOnce(const std::string& url, const std::string& json, int timeoutSeconds,
                     bool relaxUnknownCa, std::string& body, int& statusCode, DWORD& lastError)
{
    body.clear();
    statusCode = 0;
    lastError = 0;

    std::wstring wurl = Utf8ToWide(url);

    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);

    wchar_t host[256] = { 0 };
    wchar_t path[2048] = { 0 };
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);

    if (!::WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
        return false;

    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    std::wstring userAgent = L"PrintServer/" + Utf8ToWide(BuildConfig::VersionA);

    HINTERNET session = ::WinHttpOpen(userAgent.c_str(),
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
        return false;

    // 与 C# ServicePointManager.SecurityProtocol 对应：显式启用 TLS 1.0/1.1/1.2
    // （Win7 默认不含 TLS 1.2，需要该设置，且系统需安装 TLS 1.2 支持更新）
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 |
                      WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 |
                      WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    ::WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    int timeoutMs = timeoutSeconds * 1000;
    ::WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET connect = ::WinHttpConnect(session, host, uc.nPort, 0);
    if (connect == nullptr)
    {
        ::WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = ::WinHttpOpenRequest(connect, L"POST", path, nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             https ? WINHTTP_FLAG_SECURE : 0);
    if (request == nullptr)
    {
        ::WinHttpCloseHandle(connect);
        ::WinHttpCloseHandle(session);
        return false;
    }

    if (relaxUnknownCa)
    {
        DWORD flags = 0;
        DWORD flagsLen = sizeof(flags);
        if (::WinHttpQueryOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, &flagsLen))
        {
            flags |= SECURITY_FLAG_IGNORE_UNKNOWN_CA;
            ::WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
        }
    }

    const wchar_t* headers = L"Content-Type: application/json; charset=utf-8\r\n";

    BOOL sent = ::WinHttpSendRequest(request, headers, (DWORD)-1L,
                                     (LPVOID)json.data(), (DWORD)json.size(),
                                     (DWORD)json.size(), 0);

    bool gotResponse = false;

    if (sent && ::WinHttpReceiveResponse(request, nullptr))
    {
        DWORD code = 0;
        DWORD len = sizeof(code);
        if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX))
        {
            statusCode = (int)code;
        }

        for (;;)
        {
            DWORD available = 0;
            if (!::WinHttpQueryDataAvailable(request, &available) || available == 0)
                break;

            std::vector<char> chunk(available);
            DWORD read = 0;
            if (!::WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
                break;

            body.append(chunk.data(), read);
        }

        gotResponse = true;
    }

    if (!gotResponse)
        lastError = ::GetLastError();

    ::WinHttpCloseHandle(request);
    ::WinHttpCloseHandle(connect);
    ::WinHttpCloseHandle(session);

    return gotResponse;
}

// WinHTTP POST；返回 true 表示收到了 HTTP 响应（对应 C# 未抛异常）
//
// Windows 7 未更新根证书库时，系统会以"未知颁发机构"拒绝授权服务器证书
// （ERROR_WINHTTP_SECURE_FAILURE），联网激活/校验会全部失败。
// 这种情况改用内嵌根证书自行完成一次完整校验，只有校验通过才放宽
// unknown-CA 重试一次；其余失败与 C# 完全一致（返回失败 → 回落到本地校验）。
bool HttpPostRaw(const std::string& url, const std::string& json, int timeoutSeconds,
                 std::string& body, int& statusCode)
{
    std::wstring host;
    INTERNET_PORT port = 0;
    bool https = false;
    CrackTarget(url, host, port, https);

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        DWORD lastError = 0;
        if (WinHttpPostOnce(url, json, timeoutSeconds, attempt == 1, body, statusCode, lastError))
            return true;

        if (attempt == 0 && https && lastError == ERROR_WINHTTP_SECURE_FAILURE &&
            !host.empty() && TlsTrust::HostNeedsEmbeddedRootRelax(host, port))
        {
            Log::WriteLog("[授权] 系统根证书库缺少服务器证书的根，"
                          "已用内嵌根证书校验通过，放宽 unknown-CA 后重试");
            continue;
        }

        return false;
    }

    return false;
}

// 对应 C# HttpPost：非 2xx 视为失败（EnsureSuccessStatusCode 抛异常 → 返回 null）
std::string HttpPost(const std::string& url, const std::string& json, int timeoutSeconds)
{
    std::string body;
    int statusCode = 0;
    if (!HttpPostRaw(url, json, timeoutSeconds, body, statusCode))
        return std::string();

    if (statusCode < 200 || statusCode >= 300)
        return std::string();

    return body;
}

void OnStatusChanged(Status status)
{
    StatusChangedHandler handler;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        handler = g_statusChangedHandler;
    }

    if (handler)
        handler(status);
}

} // namespace Internal

// ===========================================================================
// 状态查询
// ===========================================================================

// ===== 开源免费版：授权系统关闭（BuildConfig::LicenseEnabled = false）=====
// 所有对外入口在此短路：启动即视为已授权，不联网、不读 license.dat、不惩罚。
// 保留原有实现代码以便需要时通过 BuildConfig::LicenseEnabled 恢复。

Status CurrentStatus()
{
    if (!BuildConfig::LicenseEnabled)
        return Status::Licensed;

    std::lock_guard<std::mutex> guard(g_mutex);
    return g_currentStatus;
}

std::string CurrentLicenseKey()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_currentLicenseKey;
}

std::string LicenseExpiry()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_expiresAt;
}

bool HasLocalLicenseFile()
{
    if (!BuildConfig::LicenseEnabled)
        return true; // 免费版：视为始终存在，跳过强制激活流程

    return ::GetFileAttributesW(Internal::LicenseFilePath().c_str()) != INVALID_FILE_ATTRIBUTES;
}

void ForceLicenseReactivation()
{
    if (!BuildConfig::LicenseEnabled)
        return; // 免费版：无授权状态可重置

    std::lock_guard<std::mutex> guard(g_mutex);
    g_currentStatus = Status::Unlicensed;
    g_currentLicenseKey.clear();
}

std::string MachineCode()
{
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (!g_machineCode.empty())
            return g_machineCode;
    }

    std::string code = MachineCode::GenerateMachineCode();

    std::lock_guard<std::mutex> guard(g_mutex);
    g_machineCode = code;
    return g_machineCode;
}

void SetStatusChangedHandler(StatusChangedHandler handler)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    g_statusChangedHandler = handler;
}

// ===========================================================================
// 文案
// ===========================================================================

std::string GetStatusText()
{
    switch (CurrentStatus())
    {
    case Status::Licensed:     return BuildConfig::LicenseEnabled ? "已授权" : "免费版（无需授权）";
    case Status::Unlicensed:   return "未授权";
    case Status::Expired:      return "授权已过期";
    case Status::Disabled:     return "授权已被禁用";
    case Status::NetworkError: return "网络连接失败，授权验证未通过";
    default:                   return "验证中...";
    }
}

bool IsExpiryNever()
{
    std::string value = LicenseExpiry();
    return value.empty() || value == "null";
}

std::string GetLicenseExpiryText()
{
    std::string value = LicenseExpiry();

    if (!HasExpiryValue(value))
        return "有效期: 永不过期";

    struct tm tmv;
    if (TryParseExpiry(value, tmv))
    {
        time_t expire = mktime(&tmv);
        if (expire != (time_t)-1 && expire < time(nullptr))
            return "已过期: " + FormatExpiryText(value);

        return "有效期至: " + FormatExpiryText(value);
    }

    return "有效期: " + value;
}

int GetExpiryRemainingDays()
{
    if (IsExpiryNever())
        return 9999;

    struct tm tmv;
    if (!TryParseExpiry(LicenseExpiry(), tmv))
        return -1;

    time_t expire = mktime(&tmv);
    if (expire == (time_t)-1)
        return -1;

    double days = difftime(expire, time(nullptr)) / 86400.0;
    return (int)days;
}

// ===========================================================================
// 初始化
// ===========================================================================

void Initialize()
{
    std::wstring dir = ExeDirectory();
    ::CreateDirectoryW(dir.c_str(), nullptr);

    // 从编译期常量获取服务器地址
    std::string url = Crypto::DecodeLicenseUrl();
    if (!url.empty() && url.back() != '/')
        url += "/";

    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_serverBaseUrl = url;
    }

    // 读取本地缓存
    LoadLocalLicense();
}

// ===========================================================================
// 本地缓存
// ===========================================================================

namespace Internal
{

bool ValidateLocalLicense()
{
    if (!BuildConfig::LicenseEnabled)
        return true; // 免费版：本地校验恒通过

    if (!HasLocalLicenseFile())
        return false;

    std::string encrypted = ReadTextFileRaw(LicenseFilePath());
    if (encrypted.empty())
        return false;

    std::string json = Crypto::DecryptFromBase64(encrypted);
    if (json.empty())
        return false;

    JsonValue result = JsonValue::Parse(json);
    if (result.type != JsonValue::OBJ)
        return false;

    const JsonValue* mcNode = result.Find("machine_code");
    std::string savedMachineCode = mcNode ? mcNode->ToConvertString() : "";
    if (savedMachineCode.empty())
        return false;

    bool machineMatch = (savedMachineCode == MachineCode());

    // 机器码不完全匹配时，尝试物理网卡 MAC 交集
    if (!machineMatch)
    {
        const JsonValue* macNode = result.Find("mac_list");
        std::string savedMacListStr = macNode ? macNode->ToConvertString() : "";

        if (!savedMacListStr.empty())
        {
            std::vector<std::string> currentMacList = MachineCode::GetPhysicalMacList();

            // 拆分保存的 MAC 列表
            std::vector<std::string> savedMacs;
            size_t pos = 0;
            while (pos <= savedMacListStr.size())
            {
                size_t comma = savedMacListStr.find(',', pos);
                std::string item = (comma == std::string::npos)
                    ? savedMacListStr.substr(pos)
                    : savedMacListStr.substr(pos, comma - pos);
                if (!item.empty())
                    savedMacs.push_back(item);
                if (comma == std::string::npos)
                    break;
                pos = comma + 1;
            }

            for (const std::string& current : currentMacList)
            {
                for (const std::string& saved : savedMacs)
                {
                    if (_stricmp(current.c_str(), saved.c_str()) == 0)
                    {
                        machineMatch = true;
                        break;
                    }
                }
                if (machineMatch)
                    break;
            }
        }
    }

    if (!machineMatch)
        return false;

    const JsonValue* expNode = result.Find("expires_at");
    std::string expiresAt = expNode ? expNode->ToConvertString() : "";

    if (HasExpiryValue(expiresAt) && ExpiryIsPast(expiresAt))
        return false; // 已过期

    {
        std::lock_guard<std::mutex> guard(g_mutex);
        const JsonValue* keyNode = result.Find("license_key");
        g_currentLicenseKey = keyNode ? keyNode->ToConvertString() : "";
        g_expiresAt = expiresAt;
    }

    return true;
}

bool IsServerReachable()
{
    if (!BuildConfig::LicenseEnabled)
        return false; // 免费版：无授权服务器

    std::string url;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        url = g_serverBaseUrl;
    }

    if (url.empty())
        url = Crypto::DecodeLicenseUrl();
    if (!url.empty() && url.back() != '/')
        url += "/";

    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.emplace_back("machine_code", MachineCode());
    std::string json = BuildJson(pairs);

    // 对应 C#：只要服务器有响应（无论状态码）即视为可达
    std::string body;
    int statusCode = 0;
    return HttpPostRaw(url + "validate", json, 5, body, statusCode);
}

bool ValidateWithServerSync()
{
    if (!BuildConfig::LicenseEnabled)
        return true; // 免费版：联网校验恒通过

    // 校验前先检查授权系统是否被禁用
    if (AntiDebug::IsLicenseSystemTampered())
    {
        Log::WriteLog("[授权] 检测到授权系统被篡改，执行惩罚");
        AntiDebug::ForcePenalty();
        return false;
    }

    std::string url;
    std::string licenseKey;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        url = g_serverBaseUrl;
        licenseKey = g_currentLicenseKey;
    }

    if (url.empty())
        url = Crypto::DecodeLicenseUrl();
    if (!url.empty() && url.back() != '/')
        url += "/";

    std::string machineCode = MachineCode();
    std::map<std::string, std::string> hardwareInfo = MachineCode::GetHardwareInfo(machineCode);

    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.emplace_back("license_key", licenseKey);
    pairs.emplace_back("machine_code", machineCode);
    pairs.emplace_back("hostname", hardwareInfo["hostname"]);
    pairs.emplace_back("os_version", hardwareInfo["os_version"]);
    pairs.emplace_back("dotnet_version", hardwareInfo["dotnet_version"]);
    pairs.emplace_back("cpu_id", hardwareInfo["cpu_id"]);
    pairs.emplace_back("cpu_name", hardwareInfo["cpu_name"]);
    pairs.emplace_back("motherboard_sn", hardwareInfo["motherboard_sn"]);
    pairs.emplace_back("motherboard_model", hardwareInfo["motherboard_model"]);
    pairs.emplace_back("machine_uuid", hardwareInfo["machine_uuid"]);
    pairs.emplace_back("bios_sn", hardwareInfo["bios_sn"]);
    pairs.emplace_back("disk_sn", hardwareInfo["disk_sn"]);
    pairs.emplace_back("disk_model", hardwareInfo["disk_model"]);
    pairs.emplace_back("mac_address", hardwareInfo["mac_address"]);
    pairs.emplace_back("app_version", BuildConfig::VersionA);

    std::string json = BuildJson(pairs);
    std::string response = HttpPost(url + "validate", json, 30);

    // 服务端失联 → 回退本地校验
    if (response.empty())
    {
        if (ValidateLocalLicense())
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            g_currentStatus = Status::Licensed;
            return true;
        }

        std::lock_guard<std::mutex> guard(g_mutex);
        g_currentStatus = Status::NetworkError;
        return false;
    }

    JsonValue result = JsonValue::Parse(response);
    if (result.type != JsonValue::OBJ)
    {
        if (ValidateLocalLicense())
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            g_currentStatus = Status::Licensed;
            return true;
        }

        std::lock_guard<std::mutex> guard(g_mutex);
        g_currentStatus = Status::NetworkError;
        return false;
    }

    const JsonValue* validNode = result.Find("valid");
    if (validNode != nullptr && validNode->AsBool())
    {
        std::string serverLicenseKey;
        const JsonValue* keyNode = result.Find("license_key");
        if (keyNode != nullptr)
            serverLicenseKey = keyNode->ToConvertString();

        std::string newExpiry;
        const JsonValue* expNode = result.Find("expires_at");
        if (expNode != nullptr)
        {
            std::string expiresAt = expNode->ToConvertString();
            newExpiry = HasExpiryValue(expiresAt) ? expiresAt : "";
        }

        std::string keyToSave;
        if (!serverLicenseKey.empty())
            keyToSave = serverLicenseKey;
        else if (!licenseKey.empty())
            keyToSave = licenseKey;
        else
            keyToSave = "MC-" + machineCode.substr(0, (std::min)(machineCode.size(), (size_t)16));

        {
            std::lock_guard<std::mutex> guard(g_mutex);
            g_currentStatus = Status::Licensed;
            if (expNode != nullptr)
                g_expiresAt = newExpiry;
        }

        if (!keyToSave.empty())
            SaveLocalLicense(keyToSave, "online");

        // 离线激活后首次联网，补报硬件信息
        if (HasPendingReport())
            ReportMachine();

        return true;
    }

    const JsonValue* statusNode = result.Find("status");
    std::string statusText = statusNode ? statusNode->ToConvertString() : "";

    const JsonValue* expiredNode = result.Find("expired");

    if (statusText == "expired" || (expiredNode != nullptr && expiredNode->AsBool()))
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_currentStatus = Status::Expired;
        return false;
    }

    if (statusText == "disabled")
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_currentStatus = Status::Disabled;
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_currentStatus = Status::Unlicensed;
    }

    // 服务端无此机器记录时自动上报注册硬件信息
    if (statusText == "none")
        ReportMachine();

    return false;
}

} // namespace Internal

// SaveLocalLicense / LoadLocalLicense 需要 Internal::ValidateLocalLicense 之后使用，
// 放在同一命名空间内，故此处继续实现。

void SaveLocalLicense(const std::string& licenseKey, const std::string& method)
{
    try
    {
        std::wstring dir = ExeDirectory();
        ::CreateDirectoryW(dir.c_str(), nullptr);

        // 保存当前物理网卡 MAC 列表，用于后续部分匹配
        std::vector<std::string> macList = MachineCode::GetPhysicalMacList();
        std::string macListStr;
        for (size_t i = 0; i < macList.size(); ++i)
        {
            if (i > 0)
                macListStr += ",";
            macListStr += macList[i];
        }

        std::string expiresAt;
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            expiresAt = g_expiresAt;
        }

        std::vector<std::pair<std::string, std::string>> pairs;
        pairs.emplace_back("license_key", licenseKey);
        pairs.emplace_back("machine_code", MachineCode());
        pairs.emplace_back("activate_method", method);
        pairs.emplace_back("expires_at", expiresAt);
        pairs.emplace_back("saved_at", CurrentTimestamp());
        pairs.emplace_back("mac_list", macListStr);

        std::string json = Internal::BuildJson(pairs);

        // DPAPI 加密存储
        std::string encrypted = Crypto::EncryptToBase64(json);
        if (encrypted.empty())
            return;

        WriteTextFileRaw(Internal::LicenseFilePath(), encrypted);

        std::lock_guard<std::mutex> guard(g_mutex);
        g_currentLicenseKey = licenseKey;
    }
    catch (...)
    {
    }
}

void LoadLocalLicense()
{
    if (!HasLocalLicenseFile())
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_currentStatus = Status::Unlicensed;
        return;
    }

    std::string encrypted = ReadTextFileRaw(Internal::LicenseFilePath());
    if (encrypted.empty())
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_currentStatus = Status::Unlicensed;
        return;
    }

    std::string json = Crypto::DecryptFromBase64(encrypted);
    if (json.empty())
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_currentStatus = Status::Unlicensed;
        return;
    }

    JsonValue result = JsonValue::Parse(json);
    if (result.type != JsonValue::OBJ)
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_currentStatus = Status::Unlicensed;
        return;
    }

    const JsonValue* keyNode = result.Find("license_key");
    const JsonValue* expNode = result.Find("expires_at");
    const JsonValue* mcNode = result.Find("machine_code");

    std::string licenseKey = keyNode ? keyNode->ToConvertString() : "";
    std::string expiresAt = expNode ? expNode->ToConvertString() : "";
    std::string savedMachineCode = mcNode ? mcNode->ToConvertString() : "";

    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_currentLicenseKey = licenseKey;
        g_expiresAt = expiresAt;
    }

    if (savedMachineCode == MachineCode())
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_currentStatus = Status::Unknown; // 待启动时联网校验
        return;
    }

    // 机器码不完全匹配 → 用物理网卡 MAC 交集做部分匹配
    bool partialMatch = false;

    const JsonValue* macNode = result.Find("mac_list");
    std::string savedMacListStr = macNode ? macNode->ToConvertString() : "";

    if (!savedMacListStr.empty())
    {
        std::vector<std::string> currentMacList = MachineCode::GetPhysicalMacList();

        std::vector<std::string> savedMacs;
        size_t pos = 0;
        while (pos <= savedMacListStr.size())
        {
            size_t comma = savedMacListStr.find(',', pos);
            std::string item = (comma == std::string::npos)
                ? savedMacListStr.substr(pos)
                : savedMacListStr.substr(pos, comma - pos);
            if (!item.empty())
                savedMacs.push_back(item);
            if (comma == std::string::npos)
                break;
            pos = comma + 1;
        }

        for (const std::string& current : currentMacList)
        {
            for (const std::string& saved : savedMacs)
            {
                if (_stricmp(current.c_str(), saved.c_str()) == 0)
                {
                    partialMatch = true;
                    break;
                }
            }
            if (partialMatch)
                break;
        }
    }

    if (partialMatch)
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_currentStatus = Status::Unknown;
    }
    else
    {
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            g_currentLicenseKey.clear();
            g_currentStatus = Status::Unlicensed;
        }
        ::DeleteFileW(Internal::LicenseFilePath().c_str());
        return;
    }

    // 部分匹配成功：更新本地授权文件中的机器码与 MAC 列表
    SaveLocalLicense(licenseKey, "partial_match");
}

// ===========================================================================
// 激活
// ===========================================================================

ActivationResult ActivateOnline(const std::string& licenseKey)
{
    if (!BuildConfig::LicenseEnabled)
        return ActivationResult(true, "开源免费版无需激活");

    try
    {
        std::string trimmed = TrimAscii(licenseKey);
        // 对应 C# licenseKey.Trim().ToUpperInvariant()
        for (char& c : trimmed)
        {
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 'a' + 'A');
        }

        std::string machineCode = MachineCode();
        std::map<std::string, std::string> hardwareInfo = MachineCode::GetHardwareInfo(machineCode);

        std::vector<std::pair<std::string, std::string>> pairs;
        pairs.emplace_back("license_key", trimmed);
        pairs.emplace_back("machine_code", machineCode);
        pairs.emplace_back("hostname", hardwareInfo["hostname"]);
        pairs.emplace_back("os_version", hardwareInfo["os_version"]);
        pairs.emplace_back("dotnet_version", hardwareInfo["dotnet_version"]);
        pairs.emplace_back("cpu_id", hardwareInfo["cpu_id"]);
        pairs.emplace_back("cpu_name", hardwareInfo["cpu_name"]);
        pairs.emplace_back("motherboard_sn", hardwareInfo["motherboard_sn"]);
        pairs.emplace_back("motherboard_model", hardwareInfo["motherboard_model"]);
        pairs.emplace_back("machine_uuid", hardwareInfo["machine_uuid"]);
        pairs.emplace_back("bios_sn", hardwareInfo["bios_sn"]);
        pairs.emplace_back("disk_sn", hardwareInfo["disk_sn"]);
        pairs.emplace_back("disk_model", hardwareInfo["disk_model"]);
        pairs.emplace_back("mac_address", hardwareInfo["mac_address"]);
        pairs.emplace_back("app_version", BuildConfig::VersionA);

        std::string url;
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            url = g_serverBaseUrl;
        }
        if (url.empty())
            url = Crypto::DecodeLicenseUrl();
        if (!url.empty() && url.back() != '/')
            url += "/";

        std::string response = Internal::HttpPost(url + "activate",
                                                  Internal::BuildJson(pairs), 30);

        if (response.empty())
            return ActivationResult(false, "网络请求失败，请检查网络连接");

        JsonValue result = JsonValue::Parse(response);

        const JsonValue* successNode = result.Find("success");
        if (successNode != nullptr && successNode->AsBool())
        {
            const JsonValue* dataNode = result.Find("data");
            if (dataNode != nullptr && dataNode->type == JsonValue::OBJ)
            {
                const JsonValue* expNode = dataNode->Find("expires_at");
                if (expNode != nullptr)
                {
                    std::string expiresAt = expNode->ToConvertString();
                    std::lock_guard<std::mutex> guard(g_mutex);
                    g_expiresAt = HasExpiryValue(expiresAt) ? expiresAt : "";
                }
            }

            SaveLocalLicense(trimmed, "online");

            // 清除离线待上报标记
            std::wstring pending = Internal::PendingReportPath();
            if (::GetFileAttributesW(pending.c_str()) != INVALID_FILE_ATTRIBUTES)
                ::DeleteFileW(pending.c_str());

            {
                std::lock_guard<std::mutex> guard(g_mutex);
                g_currentStatus = Status::Licensed;
            }
            Internal::OnStatusChanged(Status::Licensed);

            return ActivationResult(true, "激活成功");
        }

        // 提取错误消息
        std::string message = "激活失败";
        const JsonValue* msgNode = result.Find("message");
        if (msgNode != nullptr)
            message = msgNode->ToConvertString();

        return ActivationResult(false, message);
    }
    catch (const std::exception& ex)
    {
        return ActivationResult(false, "网络错误: " + std::string(ex.what()));
    }
    catch (...)
    {
        return ActivationResult(false, "网络错误: 未知异常");
    }
}

namespace
{

// 新版加密 .lic 格式
ActivationResult ActivateOfflineV1(const JsonValue& jsonObj)
{
    try
    {
        const JsonValue* dataNode = jsonObj.Find("data");
        const JsonValue* sigNode = jsonObj.Find("sig");

        std::string dataField = dataNode ? dataNode->ToConvertString() : "";
        std::string sigField = sigNode ? sigNode->ToConvertString() : "";

        if (dataField.empty() || sigField.empty())
            return ActivationResult(false, "无效的授权文件格式");

        std::string secret = Internal::GetDefaultSecretKey();

        std::string decryptedJson = Crypto::DecryptLicenseFile(dataField, sigField, secret);

        if (decryptedJson.empty())
        {
            // 尝试配置中的自定义密钥
            std::string configSecret = Config::GetSetting("LicenseSecretKey", secret);
            if (configSecret != secret)
                decryptedJson = Crypto::DecryptLicenseFile(dataField, sigField, configSecret);

            if (decryptedJson.empty())
                return ActivationResult(false, "授权文件签名无效，已被篡改");
        }

        JsonValue decrypted = JsonValue::Parse(decryptedJson);
        if (decrypted.type != JsonValue::OBJ)
            return ActivationResult(false, "授权文件内容无效");

        const JsonValue* keyNode = decrypted.Find("license_key");
        const JsonValue* mcNode = decrypted.Find("machine_code");
        const JsonValue* expNode = decrypted.Find("expires_at");

        std::string licenseKey = keyNode ? keyNode->ToConvertString() : "";
        std::string machineCode = mcNode ? mcNode->ToConvertString() : "";
        std::string expiresAt = expNode ? expNode->ToConvertString() : "";

        if (licenseKey.empty() || machineCode.empty())
            return ActivationResult(false, "授权文件内容无效");

        if (machineCode != MachineCode())
            return ActivationResult(false, "授权文件与本机不匹配");

        if (HasExpiryValue(expiresAt) && ExpiryIsPast(expiresAt))
            return ActivationResult(false, "授权文件已过期");

        {
            std::lock_guard<std::mutex> guard(g_mutex);
            g_expiresAt = HasExpiryValue(expiresAt) ? expiresAt : "";
        }

        SaveLocalLicense(licenseKey, "offline");

        // 标记：有网时需要补报
        WriteTextFileRaw(Internal::PendingReportPath(), CurrentTimestamp());

        {
            std::lock_guard<std::mutex> guard(g_mutex);
            g_currentStatus = Status::Licensed;
        }
        Internal::OnStatusChanged(Status::Licensed);

        return ActivationResult(true, "离线激活成功");
    }
    catch (const std::exception& ex)
    {
        return ActivationResult(false, "解析授权文件失败: " + std::string(ex.what()));
    }
}

// 旧版明文 .lic 格式（兼容旧版）
ActivationResult ActivateOfflineLegacy(const JsonValue& jsonObj)
{
    try
    {
        const JsonValue* keyNode = jsonObj.Find("license_key");
        const JsonValue* mcNode = jsonObj.Find("machine_code");
        const JsonValue* expNode = jsonObj.Find("expires_at");
        const JsonValue* sigNode = jsonObj.Find("signature");

        std::string licenseKey = keyNode ? keyNode->ToConvertString() : "";
        std::string machineCode = mcNode ? mcNode->ToConvertString() : "";
        std::string expiresAt = expNode ? expNode->ToConvertString() : "";
        std::string signature = sigNode ? sigNode->ToConvertString() : "";

        if (licenseKey.empty() || machineCode.empty())
            return ActivationResult(false, "无效的授权文件");

        if (machineCode != MachineCode())
            return ActivationResult(false, "授权文件与本机不匹配");

        std::string expiresForSign = HasExpiryValue(expiresAt) ? expiresAt : "never";

        std::string secret = Internal::GetDefaultSecretKey();
        std::string dataToVerify = licenseKey + machineCode + expiresForSign;

        if (!Crypto::VerifyHmacSignature(dataToVerify, signature, secret))
        {
            std::string configSecret = Config::GetSetting("LicenseSecretKey", secret);
            if (configSecret != secret)
            {
                if (!Crypto::VerifyHmacSignature(dataToVerify, signature, configSecret))
                    return ActivationResult(false, "授权文件签名无效，已被篡改");
            }
            else
            {
                return ActivationResult(false, "授权文件签名无效，已被篡改");
            }
        }

        if (HasExpiryValue(expiresAt) && ExpiryIsPast(expiresAt))
            return ActivationResult(false, "授权文件已过期");

        {
            std::lock_guard<std::mutex> guard(g_mutex);
            g_expiresAt = HasExpiryValue(expiresAt) ? expiresAt : "";
        }

        SaveLocalLicense(licenseKey, "offline");

        WriteTextFileRaw(Internal::PendingReportPath(), CurrentTimestamp());

        {
            std::lock_guard<std::mutex> guard(g_mutex);
            g_currentStatus = Status::Licensed;
        }
        Internal::OnStatusChanged(Status::Licensed);

        return ActivationResult(true, "离线激活成功");
    }
    catch (const std::exception& ex)
    {
        return ActivationResult(false, "读取授权文件失败: " + std::string(ex.what()));
    }
}

} // namespace

ActivationResult ActivateOffline(const std::wstring& filePath)
{
    if (!BuildConfig::LicenseEnabled)
        return ActivationResult(true, "开源免费版无需激活");

    try
    {
        // 防御性检查：外网可达时禁止离线激活
        if (Internal::IsServerReachable())
            return ActivationResult(false, "检测到外网授权服务器可达，请使用在线激活，禁止离线激活");

        std::string content = ReadTextFileRaw(filePath);
        if (TrimAscii(content).empty())
            return ActivationResult(false, "授权文件为空");

        JsonValue jsonObj = JsonValue::Parse(content);
        if (jsonObj.type != JsonValue::OBJ)
            return ActivationResult(false, "无效的授权文件格式");

        // 新版加密格式：包含 v / enc / data / sig 字段
        if (jsonObj.Has("v") && jsonObj.Has("enc") && jsonObj.Has("data") && jsonObj.Has("sig"))
            return ActivateOfflineV1(jsonObj);

        return ActivateOfflineLegacy(jsonObj);
    }
    catch (const std::exception& ex)
    {
        return ActivationResult(false, "读取授权文件失败: " + std::string(ex.what()));
    }
}

// ===========================================================================
// 上报
// ===========================================================================

bool ReportMachine()
{
    if (!BuildConfig::LicenseEnabled)
        return false; // 免费版：不上报硬件信息

    try
    {
        std::string machineCode = MachineCode();
        std::map<std::string, std::string> hardwareInfo = MachineCode::GetHardwareInfo(machineCode);

        std::vector<std::pair<std::string, std::string>> pairs;
        pairs.emplace_back("machine_code", machineCode);
        pairs.emplace_back("hostname", hardwareInfo["hostname"]);
        pairs.emplace_back("os_version", hardwareInfo["os_version"]);
        pairs.emplace_back("dotnet_version", hardwareInfo["dotnet_version"]);
        pairs.emplace_back("cpu_id", hardwareInfo["cpu_id"]);
        pairs.emplace_back("cpu_name", hardwareInfo["cpu_name"]);
        pairs.emplace_back("motherboard_sn", hardwareInfo["motherboard_sn"]);
        pairs.emplace_back("motherboard_model", hardwareInfo["motherboard_model"]);
        pairs.emplace_back("machine_uuid", hardwareInfo["machine_uuid"]);
        pairs.emplace_back("bios_sn", hardwareInfo["bios_sn"]);
        pairs.emplace_back("disk_sn", hardwareInfo["disk_sn"]);
        pairs.emplace_back("disk_model", hardwareInfo["disk_model"]);
        pairs.emplace_back("mac_address", hardwareInfo["mac_address"]);
        pairs.emplace_back("app_version", BuildConfig::VersionA);

        std::string url;
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            url = g_serverBaseUrl;
        }
        if (url.empty())
            url = Crypto::DecodeLicenseUrl();
        if (!url.empty() && url.back() != '/')
            url += "/";

        std::string response = Internal::HttpPost(url + "report",
                                                  Internal::BuildJson(pairs), 30);

        if (!response.empty())
        {
            std::wstring pending = Internal::PendingReportPath();
            if (::GetFileAttributesW(pending.c_str()) != INVALID_FILE_ATTRIBUTES)
                ::DeleteFileW(pending.c_str());

            JsonValue result = JsonValue::Parse(response);
            if (result.type == JsonValue::OBJ)
            {
                const JsonValue* dataNode = result.Find("data");
                if (dataNode != nullptr && dataNode->type == JsonValue::OBJ)
                {
                    const JsonValue* licensedNode = dataNode->Find("licensed");
                    if (licensedNode != nullptr && licensedNode->AsBool())
                    {
                        {
                            std::lock_guard<std::mutex> guard(g_mutex);
                            g_currentStatus = Status::Licensed;
                        }
                        Internal::OnStatusChanged(Status::Licensed);
                    }
                }
            }
            return true;
        }

        return false;
    }
    catch (...)
    {
        return false;
    }
}

bool HasPendingReport()
{
    return ::GetFileAttributesW(Internal::PendingReportPath().c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ===========================================================================
// 验证流程
// ===========================================================================

void StartValidationTimer()
{
    if (g_timerRunning.exchange(true))
        return; // 已在运行

    int intervalHours = Internal::GetLicenseValidationIntervalHours();
    if (intervalHours < 1)
        intervalHours = 1;

    g_timerThread = std::thread([intervalHours]() {
        // 首次延迟与周期间隔均为 intervalHours（与 C# Timer 的构造一致）
        std::unique_lock<std::mutex> lock(g_timerMutex);

        while (g_timerRunning.load())
        {
            auto status = g_timerCv.wait_for(lock, std::chrono::hours(intervalHours));
            if (status == std::cv_status::no_timeout)
                break; // 收到停止信号
            if (!g_timerRunning.load())
                break;

            lock.unlock();
            OnValidationTimer();
            lock.lock();
        }
    });
    g_timerThread.detach();
}

void OnValidationTimer()
{
    if (!BuildConfig::LicenseEnabled)
        return; // 免费版：无定时复验

    if (g_isChecking.exchange(true))
        return;

    AntiDebug::Checkpoint("TimerValidation");

    bool valid = Internal::ValidateWithServerSync();
    Status status = CurrentStatus();
    Internal::OnStatusChanged(valid ? Status::Licensed : status);

    // 服务端返回禁用或过期 → 立即惩罚
    if (status == Status::Disabled || status == Status::Expired)
    {
        Log::WriteLog("[授权] 服务端返回状态: " + GetStatusText() +
                      "，授权码: " + CurrentLicenseKey() + "，执行惩罚");
        AntiDebug::ForcePenalty();
    }

    g_isChecking.store(false);
}

bool StartValidation()
{
    if (!BuildConfig::LicenseEnabled)
    {
        Internal::OnStatusChanged(Status::Licensed);
        return true; // 免费版：启动即授权通过，不启动验证定时器
    }

    AntiDebug::Checkpoint("StartValidation");

    // 第一次：强制联网校验
    bool valid = Internal::ValidateWithServerSync();

    StartValidationTimer();

    Internal::OnStatusChanged(valid ? Status::Licensed : CurrentStatus());
    return valid;
}

void StopValidation()
{
    if (!g_timerRunning.exchange(false))
        return;

    g_timerCv.notify_all();
}

bool ManualCheck()
{
    if (!BuildConfig::LicenseEnabled)
        return true; // 免费版：手动检查恒通过

    if (g_isChecking.exchange(true))
        return false;

    bool valid = Internal::ValidateWithServerSync();
    Internal::OnStatusChanged(valid ? Status::Licensed : CurrentStatus());

    g_isChecking.store(false);
    return valid;
}

}
}
