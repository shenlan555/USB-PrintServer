#include "pch.h"
#include "PrintService.h"
#include "Config.h"
#include "LogHelper.h"
#include "Wmi.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <winspool.h>
#include <stdio.h>
#include <string.h>
#include <stdexcept>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winspool.lib")

namespace ps
{

// ===========================================================================
// ConfigurationHelper
// ===========================================================================

namespace ConfigHelper
{

std::string GetSetting(const std::string& key, const std::string& defaultValue)
{
    return Config::GetSetting(key, defaultValue);
}

namespace
{

bool ParseBoolSetting(const std::string& text)
{
    std::string lower;
    lower.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        char c = text[i];
        lower.push_back((c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c);
    }
    return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
}

} // namespace

std::vector<PrinterSlot> GetPrinterSlots()
{
    std::vector<PrinterSlot> slots;

    const int count = atoi(GetSetting("PrinterCount", "0").c_str());

    if (count <= 0)
    {
        // 兼容旧单打印机模式
        const std::string name = GetSetting("PrinterName", "");
        if (!name.empty())
        {
            PrinterSlot slot;
            slot.slot = 0;
            slot.name = name;
            slot.port = atoi(GetSetting("Port", "9100").c_str());
            slot.enabled = true;
            slots.push_back(slot);
        }
        return slots;
    }

    for (int i = 0; i < count; ++i)
    {
        const std::string prefix = "Printer" + std::to_string(i) + "_";
        const std::string name = GetSetting(prefix + "Name", "");
        if (name.empty())
            continue;

        PrinterSlot slot;
        slot.slot = i;
        slot.name = name;
        slot.port = atoi(GetSetting(prefix + "Port", std::to_string(9100 + i)).c_str());
        slot.enabled = ParseBoolSetting(GetSetting(prefix + "Enabled", "false"));
        slots.push_back(slot);
    }

    return slots;
}

std::vector<PrinterConfig> GetPrinterConfigs()
{
    std::vector<PrinterConfig> configs;

    int count = atoi(GetSetting("PrinterCount", "0").c_str());

    if (count <= 0)
    {
        // 兼容旧单打印机模式
        std::string name = GetSetting("PrinterName", "");
        if (!name.empty())
        {
            PrinterConfig cfg;
            cfg.name = name;
            cfg.port = atoi(GetSetting("Port", "9100").c_str());
            cfg.enabled = true;
            // 与多打印机模式一致：编号 = RAW 端口号（9100 -> /ipp/print/9100）
            cfg.index = (cfg.port >= 9100) ? cfg.port : 0;
            configs.push_back(cfg);
        }
        return configs;
    }

    // 多打印机模式
    for (int i = 0; i < count; ++i)
    {
        std::string prefix = "Printer" + std::to_string(i) + "_";

        std::string name = GetSetting(prefix + "Name", "");
        if (name.empty())
            continue;

        std::string enabledStr = GetSetting(prefix + "Enabled", "false");
        std::string enabledLower;
        for (char c : enabledStr)
            enabledLower.push_back((c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c);

        bool enabled = (enabledLower == "true" || enabledStr == "1");
        if (!enabled)
            continue;

        PrinterConfig cfg;
        cfg.name = name;
        cfg.port = atoi(GetSetting(prefix + "Port", std::to_string(9100 + i)).c_str());
        cfg.enabled = true;
        // 与 GUI 保持一致：编号 = RAW 端口号，端口 9100+i 对应 /ipp/print/9100+i
        cfg.index = (cfg.port >= 9100) ? cfg.port : (9100 + i);
        configs.push_back(cfg);
    }

    return configs;
}

} // namespace ConfigHelper

// ===========================================================================
// NetworkHelper
// ===========================================================================

namespace NetworkHelper
{

// WinSock 初始化（进程内只需一次）
void EnsureWinsock()
{
    static std::once_flag s_once;
    std::call_once(s_once, []() {
        WSADATA wsa;
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}

std::vector<std::string> GetAllNetworkAddresses()
{
    EnsureWinsock();

    std::vector<std::string> addresses;

    char hostName[256] = { 0 };
    if (::gethostname(hostName, sizeof(hostName)) != 0)
    {
        addresses.push_back("127.0.0.1");
        return addresses;
    }

    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        // 只取 IPv4（与 C# 的 InterNetwork 过滤一致）
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    if (::getaddrinfo(hostName, nullptr, &hints, &result) != 0 || result == nullptr)
    {
        addresses.push_back("127.0.0.1");
        return addresses;
    }

    for (addrinfo* p = result; p != nullptr; p = p->ai_next)
    {
        if (p->ai_family != AF_INET)
            continue;

        char text[INET_ADDRSTRLEN] = { 0 };
        sockaddr_in* addr = (sockaddr_in*)p->ai_addr;
        if (::inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) != nullptr)
        {
            std::string ip = text;
            bool exists = false;
            for (const auto& a : addresses)
            {
                if (a == ip)
                {
                    exists = true;
                    break;
                }
            }
            if (!exists)
                addresses.push_back(ip);
        }
    }

    ::freeaddrinfo(result);

    if (addresses.empty())
        addresses.push_back("127.0.0.1");

    return addresses;
}

std::string GetHostName(const std::string& ipAddress)
{
    EnsureWinsock();

    // 双栈套接字上 IPv4 客户端会呈现为 IPv4 映射地址（::ffff:a.b.c.d），
    // 先还原为点分 IPv4 再反查，以贴近 C# Dns.GetHostEntry 的行为
    std::string ip = ipAddress;
    const std::string mappedPrefix = "::ffff:";
    if (ip.size() > mappedPrefix.size() &&
        ip.compare(0, mappedPrefix.size(), mappedPrefix) == 0)
    {
        ip = ip.substr(mappedPrefix.size());
    }

    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;

    if (::inet_pton(AF_INET, ip.c_str(), &sa.sin_addr) != 1)
        return ipAddress;

    char host[NI_MAXHOST] = { 0 };
    if (::getnameinfo((sockaddr*)&sa, sizeof(sa), host, sizeof(host),
                      nullptr, 0, NI_NAMEREQD) != 0)
    {
        return ipAddress;
    }

    return host;
}

bool ResolveHostName(const std::string& hostName, std::vector<std::string>& ipAddresses)
{
    EnsureWinsock();

    ipAddresses.clear();

    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    if (::getaddrinfo(hostName.c_str(), nullptr, &hints, &result) != 0 || result == nullptr)
        return false;

    for (addrinfo* p = result; p != nullptr; p = p->ai_next)
    {
        if (p->ai_family != AF_INET)
            continue;

        char text[INET_ADDRSTRLEN] = { 0 };
        sockaddr_in* addr = (sockaddr_in*)p->ai_addr;
        if (::inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) != nullptr)
            ipAddresses.push_back(text);
    }

    ::freeaddrinfo(result);
    return !ipAddresses.empty();
}

} // namespace NetworkHelper

// ===========================================================================
// RawPrinterHelper
// ===========================================================================

namespace RawPrinter
{

bool SendBytesToPrinter(const std::string& printerNameUtf8,
                        const std::vector<unsigned char>& data)
{
    std::wstring printerName = Utf8ToWide(printerNameUtf8);

    HANDLE hPrinter = nullptr;
    std::wstring docName = L"网络打印作业";
    std::wstring dataType = L"RAW";

    DOC_INFO_1W docInfo;
    docInfo.pDocName = const_cast<LPWSTR>(docName.c_str());
    docInfo.pOutputFile = nullptr;
    docInfo.pDatatype = const_cast<LPWSTR>(dataType.c_str());

    if (!::OpenPrinterW(const_cast<LPWSTR>(printerName.c_str()), &hPrinter, nullptr))
        return false;

    if (!::StartDocPrinterW(hPrinter, 1, (LPBYTE)&docInfo))
    {
        ::ClosePrinter(hPrinter);
        return false;
    }

    if (!::StartPagePrinter(hPrinter))
    {
        ::EndDocPrinter(hPrinter);
        ::ClosePrinter(hPrinter);
        return false;
    }

    DWORD bytesWritten = 0;
    BOOL ok = TRUE;

    if (!data.empty())
    {
        ok = ::WritePrinter(hPrinter, (LPVOID)data.data(), (DWORD)data.size(), &bytesWritten);
    }

    ::EndPagePrinter(hPrinter);
    ::EndDocPrinter(hPrinter);
    ::ClosePrinter(hPrinter);

    return ok != FALSE;
}

// IPP 用：从文件指定区间分块流式打印。
// 与 SendBytesToPrinter 的区别：不把文档读进内存，并支持中途取消。
bool SendFileRangeToPrinter(const std::string& printerNameUtf8,
                            const std::wstring& filePath,
                            long long offset, long long length,
                            const std::function<bool()>& shouldCancel,
                            bool& canceled)
{
    canceled = false;

    if (length <= 0)
        return false;

    HANDLE hFile = ::CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER position;
    position.QuadPart = offset;
    if (!::SetFilePointerEx(hFile, position, nullptr, FILE_BEGIN))
    {
        ::CloseHandle(hFile);
        return false;
    }

    std::wstring printerName = Utf8ToWide(printerNameUtf8);
    std::wstring docName = L"IPP 打印作业";
    std::wstring dataType = L"RAW";

    DOC_INFO_1W docInfo;
    docInfo.pDocName = const_cast<LPWSTR>(docName.c_str());
    docInfo.pOutputFile = nullptr;
    docInfo.pDatatype = const_cast<LPWSTR>(dataType.c_str());

    HANDLE hPrinter = nullptr;
    if (!::OpenPrinterW(const_cast<LPWSTR>(printerName.c_str()), &hPrinter, nullptr))
    {
        ::CloseHandle(hFile);
        return false;
    }

    bool started = false;
    bool ok = true;

    do
    {
        if (!::StartDocPrinterW(hPrinter, 1, (LPBYTE)&docInfo))
        {
            ok = false;
            break;
        }

        if (!::StartPagePrinter(hPrinter))
        {
            ::EndDocPrinter(hPrinter);
            ok = false;
            break;
        }

        started = true;

        std::vector<unsigned char> buffer(64 * 1024);
        long long remaining = length;

        while (remaining > 0)
        {
            if (shouldCancel && shouldCancel())
            {
                canceled = true;
                ok = false;
                break;
            }

            DWORD toRead = (remaining < (long long)buffer.size())
                               ? (DWORD)remaining
                               : (DWORD)buffer.size();

            DWORD read = 0;
            if (!::ReadFile(hFile, buffer.data(), toRead, &read, nullptr) || read == 0)
            {
                ok = false;
                break;
            }

            DWORD written = 0;
            if (!::WritePrinter(hPrinter, buffer.data(), read, &written) || written != read)
            {
                ok = false;
                break;
            }

            remaining -= read;
        }

        if (!ok)
            break;

        if (!::EndPagePrinter(hPrinter))
        {
            ok = false;
            break;
        }

        if (!::EndDocPrinter(hPrinter))
            ok = false;
    } while (false);

    // 取消或失败时丢弃整个作业，避免只打出半页乱码
    if (started && !ok)
        ::AbortPrinter(hPrinter);

    ::ClosePrinter(hPrinter);
    ::CloseHandle(hFile);

    return ok;
}

} // namespace RawPrinter

// ===========================================================================
// 事件日志
// ===========================================================================

void WriteEventLog(const std::string& message, bool isError)
{
    // 对应 C#：EventLog.Source = "PrintServer"，Log = "Application"
    HANDLE hEventLog = ::RegisterEventSourceW(nullptr, L"PrintServer");
    if (hEventLog == nullptr)
        return;

    std::wstring wide = Utf8ToWide(message);
    LPCWSTR strings[1] = { wide.c_str() };

    ::ReportEventW(hEventLog,
                   isError ? EVENTLOG_ERROR_TYPE : EVENTLOG_INFORMATION_TYPE,
                   0, 0, nullptr, 1, 0, strings, nullptr);

    ::DeregisterEventSource(hEventLog);
}

// ===========================================================================
// TcpPrintServer
// ===========================================================================

namespace
{

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

    // 对应 C# 的 "{0:0.##}"：最多两位小数，且去掉末尾多余的 0 与小数点
    char buf[64] = { 0 };
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.2f", size);

    std::string text = buf;
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();

    return text + " " + sizes[order];
}

std::string NowText()
{
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char buf[32] = { 0 };
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring UniqueTempPrnPath()
{
    wchar_t tempDir[MAX_PATH] = { 0 };
    ::GetTempPathW(MAX_PATH, tempDir);

    wchar_t tempName[MAX_PATH] = { 0 };
    ::GetTempFileNameW(tempDir, L"psp", 0, tempName);
    ::DeleteFileW(tempName); // GetTempFileName 会创建占位文件，这里不要它

    return std::wstring(tempName) + L".prn";
}

// ---------------------------------------------------------------------------
// 驱动状态查询（PJL）应答 + 作业判定
//
// HP 等驱动在发作业前会先发一条状态查询（@PJL INFO ID 之类）并等待回应，
// 收不到回应就直接复位连接（服务端表现为 recv 10054、一个字节都没有）。
// 这里只回应「查询类」命令；SET / JOB / ENTER LANGUAGE 属于真实作业，照常打印。
// ---------------------------------------------------------------------------

const char kUelMagic[] = "\x1B%-12345X";
const size_t kUelMagicLen = 9;

enum class ProbeResult
{
    NeedMore,   // 数据不足，等下一段
    Handled,    // 已消费掉一段（查询应答 / 纯 UEL），继续解析
    Job         // 这是作业数据
};

std::string ToUpperAscii(const std::string& text)
{
    std::string out = text;
    for (char& c : out)
    {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
    }
    return out;
}

bool StartsWithNoCase(const std::vector<unsigned char>& buf, size_t off, const char* text)
{
    const size_t n = strlen(text);
    if (buf.size() - off < n)
        return false;

    for (size_t i = 0; i < n; ++i)
    {
        char a = (char)buf[off + i];
        char b = text[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b)
            return false;
    }
    return true;
}

bool IsPrefixOfNoCase(const std::vector<unsigned char>& buf, size_t off, const char* text)
{
    const size_t n = strlen(text);
    if (buf.size() - off >= n)
        return false;

    for (size_t i = 0; off + i < buf.size(); ++i)
    {
        char a = (char)buf[off + i];
        char b = text[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b)
            return false;
    }
    return true;
}

bool FindCrlf(const std::vector<unsigned char>& buf, size_t off, size_t& pos)
{
    for (size_t i = off; i + 1 < buf.size(); ++i)
    {
        if (buf[i] == '\r' && buf[i + 1] == '\n')
        {
            pos = i;
            return true;
        }
    }
    return false;
}

// 返回空串表示这不是查询命令（应视为作业数据）
std::string BuildProbeReply(const std::string& command, const std::string& printerName)
{
    const std::string upper = ToUpperAscii(command);
    std::string body;

    if (upper.rfind("INFO ID", 0) == 0)
        body = "INFO ID\r\n\"" + printerName + "\"";
    else if (upper.rfind("INFO STATUS", 0) == 0)
        body = "INFO STATUS\r\nCODE=10001\r\nDISPLAY=\"Ready\"\r\nONLINE=TRUE";
    else if (upper.rfind("INFO CONFIG", 0) == 0)
        body = "INFO CONFIG\r\nLANGUAGE=PCL";
    else if (upper.rfind("USTATUS", 0) == 0)
        body = "USTATUS DEVICE=ON\r\nUSTATUS JOB=ON";
    else if (upper.rfind("ECHO", 0) == 0)
        body = command;
    else if (upper.rfind("INQUIRE", 0) == 0)
        body = "INQUIRE";
    else
        return std::string();

    return std::string(kUelMagic) + "@PJL " + body + "\r\n" + kUelMagic;
}

// 解析缓冲区头部：查询则回包并消费，作业则原样交给调用方
ProbeResult ParseProbe(const std::vector<unsigned char>& pending, SOCKET socket,
                       const std::string& printerName, size_t& consumed, bool& replied)
{
    consumed = 0;
    replied = false;
    size_t off = 0;

    for (;;)
    {
        if (pending.size() - off >= kUelMagicLen &&
            memcmp(pending.data() + off, kUelMagic, kUelMagicLen) == 0)
        {
            off += kUelMagicLen;
            consumed = off;
            continue;
        }

        // 可能是 UEL 的开头，等更多数据
        if (pending.size() - off < kUelMagicLen &&
            memcmp(pending.data() + off, kUelMagic, pending.size() - off) == 0)
        {
            return ProbeResult::NeedMore;
        }
        break;
    }

    if (off >= pending.size())
        return (off > 0) ? ProbeResult::Handled : ProbeResult::NeedMore;

    if (!StartsWithNoCase(pending, off, "@PJL"))
        return IsPrefixOfNoCase(pending, off, "@PJL") ? ProbeResult::NeedMore : ProbeResult::Job;

    size_t crlf = 0;
    if (!FindCrlf(pending, off, crlf))
    {
        if (pending.size() - off > 1024)
            return ProbeResult::Job;
        return ProbeResult::NeedMore;
    }

    const std::string line((const char*)pending.data() + off + 4,
                           crlf - (off + 4));      // 去掉 "@PJL"
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos)
        return ProbeResult::Job;

    const std::string reply = BuildProbeReply(line.substr(first), printerName);
    if (reply.empty())
        return ProbeResult::Job;                    // SET / JOB / ENTER LANGUAGE

    ::send(socket, reply.data(), (int)reply.size(), 0);
    replied = true;
    consumed = crlf + 2;
    return ProbeResult::Handled;
}

// ---------------------------------------------------------------------------
// 作业指纹：只对「真正要打印的数据」算哈希
//
// 驱动重试时 PJL 头里的 JOB NAME / 时间戳会变，整包哈希每次都不一样，
// 只按整包比对挡不住重复打印。这里跳过 ENTER LANGUAGE 之前的部分，
// 用后面 PCL/PS 正文算指纹，重试的作业指纹一致。
// ---------------------------------------------------------------------------

unsigned long long HashJobPayload(const std::wstring& filePath)
{
    HANDLE hFile = ::CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return 0;

    LARGE_INTEGER size = { 0 };
    ::GetFileSizeEx(hFile, &size);

    const long long kMaxBytes = 64LL * 1024 * 1024;
    long long want = size.QuadPart;
    if (want > kMaxBytes)
        want = kMaxBytes;

    std::vector<char> data((size_t)want);
    DWORD read = 0;
    if (want > 0)
        ::ReadFile(hFile, data.data(), (DWORD)want, &read, nullptr);
    ::CloseHandle(hFile);

    data.resize(read);
    if (data.empty())
        return 0;

    const std::string text(data.begin(), data.end());

    size_t begin = 0;
    const size_t marker = text.find("ENTER LANGUAGE");
    if (marker != std::string::npos)
    {
        const size_t lineEnd = text.find("\r\n", marker);
        begin = (lineEnd == std::string::npos) ? marker + 14 : lineEnd + 2;
        if (begin > text.size())
            begin = text.size();
    }

    unsigned long long hash = 1469598103934665603ULL;
    for (size_t i = begin; i < text.size(); ++i)
    {
        hash ^= (unsigned char)text[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

TcpPrintServer::TcpPrintServer(const std::string& printerName, int port)
    : m_printerName(printerName)
    , m_port(port)
    , m_running(false)
    , m_listenSocket((uintptr_t)INVALID_SOCKET)
{
}

TcpPrintServer::~TcpPrintServer()
{
    Stop();
}

void TcpPrintServer::Log(const std::string& message)
{
    std::string line = "[" + NowText() + "] " + message;
    if (OnLog)
        OnLog(line);
}

void TcpPrintServer::Start()
{
    NetworkHelper::EnsureWinsock();

    // 双栈监听：对应 C# 的 TcpListener(IPAddress.IPv6Any) + IPv6Only=false
    int family = AF_INET6;
    SOCKET listenSocket = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET)
    {
        // 无 IPv6 时回退到 IPv4
        family = AF_INET;
        listenSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket == INVALID_SOCKET)
        {
            Log("" + m_printerName + " 启动失败: 无法创建套接字");
            throw std::runtime_error("socket 创建失败");
        }
    }
    else
    {
        // 关闭 IPv6Only，使同一端口同时接受 IPv4 客户端
        DWORD v6Only = 0;
        ::setsockopt(listenSocket, IPPROTO_IPV6, IPV6_V6ONLY,
                     (const char*)&v6Only, sizeof(v6Only));
    }

    int reuse = 1;
    ::setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    // 按套接字实际协议族构造绑定地址
    sockaddr_storage bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    int bindLen = 0;

    if (family == AF_INET6)
    {
        sockaddr_in6* a6 = (sockaddr_in6*)&bindAddr;
        a6->sin6_family = AF_INET6;
        a6->sin6_port = ::htons((u_short)m_port);
        a6->sin6_addr = in6addr_any;
        bindLen = sizeof(sockaddr_in6);
    }
    else
    {
        sockaddr_in* a4 = (sockaddr_in*)&bindAddr;
        a4->sin_family = AF_INET;
        a4->sin_port = ::htons((u_short)m_port);
        a4->sin_addr.s_addr = INADDR_ANY;
        bindLen = sizeof(sockaddr_in);
    }

    if (::bind(listenSocket, (sockaddr*)&bindAddr, bindLen) == SOCKET_ERROR)
    {
        int err = ::WSAGetLastError();
        ::closesocket(listenSocket);
        Log("" + m_printerName + " 启动失败: 端口 " + std::to_string(m_port) +
            " 绑定失败 (错误 " + std::to_string(err) + ")");
        throw std::runtime_error("bind 失败");
    }

    if (::listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        ::closesocket(listenSocket);
        Log("" + m_printerName + " 启动失败: listen 失败");
        throw std::runtime_error("listen 失败");
    }

    m_listenSocket = (uintptr_t)listenSocket;
    m_running.store(true);

    Log("" + m_printerName + " → 端口 " + std::to_string(m_port) + " 已启动");

    m_listenThread = std::thread([this]() { ListenLoop(); });
}

void TcpPrintServer::Stop()
{
    if (!m_running.exchange(false))
        return;

    SOCKET listenSocket = (SOCKET)m_listenSocket;
    if (listenSocket != INVALID_SOCKET)
    {
        ::closesocket(listenSocket);
        m_listenSocket = (uintptr_t)INVALID_SOCKET;
    }

    {
        std::lock_guard<std::mutex> guard(m_clientsMutex);
        for (uintptr_t s : m_activeClients)
            ::closesocket((SOCKET)s);
        m_activeClients.clear();
    }

    if (m_listenThread.joinable())
        m_listenThread.join();

    Log("打印服务器已停止");
}

void TcpPrintServer::ListenLoop()
{
    while (m_running.load())
    {
        SOCKET listenSocket = (SOCKET)m_listenSocket;
        if (listenSocket == INVALID_SOCKET)
            break;

        SOCKET clientSocket = ::accept(listenSocket, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET)
        {
            if (!m_running.load())
                break;
            Log("监听Socket异常");
            continue;
        }

        {
            std::lock_guard<std::mutex> guard(m_clientsMutex);
            m_activeClients.push_back((uintptr_t)clientSocket);
            if (OnClientCountChanged)
                OnClientCountChanged((int)m_activeClients.size());
        }

        std::thread([this, clientSocket]() { HandleClient((uintptr_t)clientSocket); }).detach();
    }
}

void TcpPrintServer::HandleClient(uintptr_t clientSocketValue)
{
    SOCKET clientSocket = (SOCKET)clientSocketValue;
    std::string clientAddress;

    try
    {
        sockaddr_storage peer;
        memset(&peer, 0, sizeof(peer));
        int peerLen = sizeof(peer);
        unsigned short peerPort = 0;
        const ULONGLONG startedAt = ::GetTickCount64();

        if (::getpeername(clientSocket, (sockaddr*)&peer, &peerLen) == 0)
        {
            char text[INET6_ADDRSTRLEN] = { 0 };
            if (peer.ss_family == AF_INET6)
            {
                sockaddr_in6* a6 = (sockaddr_in6*)&peer;
                ::inet_ntop(AF_INET6, &a6->sin6_addr, text, sizeof(text));
            }
            else
            {
                sockaddr_in* a4 = (sockaddr_in*)&peer;
                ::inet_ntop(AF_INET, &a4->sin_addr, text, sizeof(text));
            }
            clientAddress = text;

            const unsigned short rawPort = (peer.ss_family == AF_INET6)
                ? ((sockaddr_in6*)&peer)->sin6_port
                : ((sockaddr_in*)&peer)->sin_port;
            peerPort = ::ntohs(rawPort);
        }

        Log("收到连接: " + clientAddress + ":" + std::to_string(peerPort));

        int timeoutSec = atoi(ConfigHelper::GetSetting("ReceiveTimeout", "30").c_str());
        if (timeoutSec <= 0)
            timeoutSec = 30;

        DWORD timeoutMs = (DWORD)timeoutSec * 1000;
        ::setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

        std::wstring tempFile = UniqueTempPrnPath();
        long long totalBytes = 0;
        bool readFailed = false;

        HANDLE hFile = ::CreateFileW(tempFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            Log("无法创建临时文件: " + WideToUtf8(tempFile));
            RemoveActiveClient(clientSocketValue);
            return;
        }

        // 作业内容哈希：用于识别驱动重试造成的重复打印
        unsigned long long jobHash = 1469598103934665603ULL;

        auto writeJobBytes = [&](const unsigned char* data, size_t length) {
            if (length == 0)
                return;

            DWORD written = 0;
            ::WriteFile(hFile, data, (DWORD)length, &written, nullptr);
            totalBytes += (long long)length;

            for (size_t i = 0; i < length; ++i)
            {
                jobHash ^= data[i];
                jobHash *= 1099511628211ULL;
            }
        };

        std::vector<unsigned char> pending;   // 尚未判定为作业的头部数据
        bool jobStarted = false;

        std::vector<unsigned char> buffer(8192);
        for (;;)
        {
            int bytesRead = ::recv(clientSocket, (char*)buffer.data(), (int)buffer.size(), 0);
            if (bytesRead == 0)
                break; // 客户端关闭连接

            if (bytesRead == SOCKET_ERROR)
            {
                readFailed = true;
                break;
            }

            if (!jobStarted)
            {
                pending.insert(pending.end(), buffer.begin(), buffer.begin() + bytesRead);

                for (;;)
                {
                    size_t consumed = 0;
                    bool replied = false;
                    const ProbeResult result = ParseProbe(pending, clientSocket, m_printerName, consumed, replied);

                    if (replied)
                        Log("已回应驱动状态查询: " + clientAddress + ":" + std::to_string(peerPort));

                    if (result == ProbeResult::NeedMore)
                        break;

                    if (result == ProbeResult::Job)
                    {
                        jobStarted = true;
                        writeJobBytes(pending.data(), pending.size());
                        pending.clear();
                        break;
                    }

                    pending.erase(pending.begin(), pending.begin() + consumed);
                    if (pending.empty())
                        break;
                }
            }
            else
            {
                writeJobBytes(buffer.data(), (size_t)bytesRead);

                // 正文以 UEL 结尾 = 这份作业已经完整；短暂确认没有后续就立刻收尾打印，
                // 不再干等 ReceiveTimeout（驱动等太久会重发，表现为重复打印）
                if (bytesRead >= (int)kUelMagicLen &&
                    memcmp(buffer.data() + bytesRead - kUelMagicLen, kUelMagic, kUelMagicLen) == 0)
                {
                    fd_set readSet;
                    FD_ZERO(&readSet);
                    FD_SET(clientSocket, &readSet);

                    timeval wait = { 0, 400 * 1000 };
                    if (::select(0, &readSet, nullptr, nullptr, &wait) <= 0)
                        break;
                }
            }
        }

        ::CloseHandle(hFile);

        if (totalBytes == 0)
        {
            // 只做了状态查询、没有作业数据：不打印，也不刷错误日志
            Log("连接 " + clientAddress + ":" + std::to_string(peerPort) + " 仅查询状态，无打印数据");
            ::DeleteFileW(tempFile.c_str());
            RemoveActiveClient(clientSocketValue);
            return;
        }

        if (readFailed)
        {
            int err = ::WSAGetLastError();

            if (!jobStarted)
            {
                Log("客户端 " + clientAddress + ":" + std::to_string(peerPort) + " 提前断开（错误 " +
                    std::to_string(err) + "，未收到完整数据，" +
                    std::to_string(::GetTickCount64() - startedAt) + " ms）");
                ::DeleteFileW(tempFile.c_str());
                RemoveActiveClient(clientSocketValue);
                return;
            }

            // 作业已开始传输后对端复位：按已收到的数据继续打印（部分驱动发完就断）
            Log("客户端 " + clientAddress + ":" + std::to_string(peerPort) + " 传输中断 (错误 " +
                std::to_string(err) + ")，已收到 " + FormatSize(totalBytes) + "，继续打印");
        }

        Log("接收数据: " + FormatSize(totalBytes) + " 来自 " + clientAddress);

        // 同一台打印机短时间内重复收到相同作业：驱动重试，只打印一次。
        // 指纹取自正文（跳过 ENTER LANGUAGE 之前的 PJL 头），
        // 因为重试时 JOB NAME / 时间戳会变，整包哈希挡不住。
        {
            const unsigned long long payloadHash = HashJobPayload(tempFile);

            std::lock_guard<std::mutex> guard(m_jobMutex);
            const ULONGLONG now = ::GetTickCount64();

            if (payloadHash != 0 && m_lastJobHash == payloadHash &&
                m_lastJobTick != 0 && now - m_lastJobTick < 30000)
            {
                Log("重复作业（内容与上次相同，间隔 " +
                    std::to_string(now - m_lastJobTick) + " ms）已忽略: " + clientAddress);
                ::DeleteFileW(tempFile.c_str());
                RemoveActiveClient(clientSocketValue);
                return;
            }

            if (payloadHash != 0)
            {
                m_lastJobHash = payloadHash;
                m_lastJobTick = now;
            }
        }

        PrintTempFile(tempFile);

        Log("打印完成: " + clientAddress);

        ::DeleteFileW(tempFile.c_str());
    }
    catch (const std::exception& ex)
    {
        Log("处理客户端 " + clientAddress + " 错误: " + ex.what());
    }
    catch (...)
    {
        Log("处理客户端 " + clientAddress + " 错误: 未知异常");
    }

    RemoveActiveClient(clientSocketValue);
}

void TcpPrintServer::RemoveActiveClient(uintptr_t clientSocketValue)
{
    // 关闭套接字并从活动列表移除（保证每个连接只关闭一次）
    SOCKET clientSocket = (SOCKET)clientSocketValue;
    if (clientSocket != INVALID_SOCKET)
        ::closesocket(clientSocket);

    std::lock_guard<std::mutex> guard(m_clientsMutex);
    for (size_t i = 0; i < m_activeClients.size(); ++i)
    {
        if (m_activeClients[i] == clientSocketValue)
        {
            m_activeClients.erase(m_activeClients.begin() + i);
            break;
        }
    }

    if (OnClientCountChanged)
        OnClientCountChanged((int)m_activeClients.size());
}

bool TcpPrintServer::PrintTempFile(const std::wstring& filePath)
{
    HANDLE hFile = ::CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        Log("打印失败: 无法打开临时文件");
        return false;
    }

    LARGE_INTEGER size = { 0 };
    ::GetFileSizeEx(hFile, &size);

    std::vector<unsigned char> data((size_t)size.QuadPart);
    DWORD read = 0;
    if (!data.empty())
        ::ReadFile(hFile, data.data(), (DWORD)data.size(), &read, nullptr);
    ::CloseHandle(hFile);

    data.resize(read);

    if (!RawPrinter::SendBytesToPrinter(m_printerName, data))
    {
        Log("打印失败: 无法发送到打印机 " + m_printerName);
        return false;
    }

    return true;
}

} // namespace ps
