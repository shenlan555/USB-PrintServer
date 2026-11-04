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
// PJL 状态查询
//
// 很多 Windows 驱动在真正发作业前，会先发一段 PJL 查询（INFO STATUS / USTATUS），
// 等打印机回应后才开始流式发送数据。RAW 9100 服务器若不作答，驱动等不到回复就
// 直接复位连接，服务端日志只剩「接收数据失败 10054」，作业数据一个字节都收不到。
// 这里只应答「查询类」PJL；带 ENTER LANGUAGE / JOB 的是真作业，照常打印。
// ---------------------------------------------------------------------------

// 只认「查询类」PJL：INFO / USTATUS / ECHO / INQUIRE。
// 普通作业里的 @PJL SET / ENTER LANGUAGE 不在此列，避免把作业当成查询吃掉。
bool LooksLikePjlQuery(const std::vector<unsigned char>& head)
{
    if (head.size() < 8)
        return false;

    const std::string text(head.begin(), head.end());
    return text.find("@PJL INFO") != std::string::npos ||
           text.find("@PJL USTATUS") != std::string::npos ||
           text.find("@PJL ECHO") != std::string::npos ||
           text.find("@PJL INQUIRE") != std::string::npos;
}

bool IsRealPrintJob(const std::string& text)
{
    return text.find("ENTER LANGUAGE") != std::string::npos ||
           text.find("@PJL JOB") != std::string::npos;
}

std::string BuildPjlResponse(const std::string& request)
{
    std::string body;

    if (request.find("INFO STATUS") != std::string::npos)
    {
        body += "@PJL INFO STATUS\r\n"
                "CODE=10001\r\n"
                "DISPLAY=\"Ready\"\r\n"
                "ONLINE=TRUE\r\n";
    }

    if (request.find("INFO ID") != std::string::npos)
        body += "@PJL INFO ID\r\n\"USB-PrintServer\"\r\n";

    if (request.find("USTATUS") != std::string::npos)
    {
        body += "@PJL USTATUS DEVICE = ON\r\n"
                "@PJL USTATUS JOB = ON\r\n"
                "@PJL USTATUS PAGE = OFF\r\n";
    }

    if (body.empty())
        body = "@PJL INFO STATUS\r\nCODE=10001\r\nDISPLAY=\"Ready\"\r\n";

    return std::string("\x1b%-12345X") + body + "\x0c\x1b%-12345X";
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
        }

        // 反查主机会阻塞在 DNS 上（局域网常见数秒无响应），必须放到读取之后：
        // 否则驱动等不到响应会直接复位连接，日志里只剩「接收数据失败 10054」。
        Log("收到连接: " + clientAddress);

        // 从配置读取接收超时（秒），默认 30 秒
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

        std::vector<unsigned char> buffer(8192);
        // 保留首段数据：驱动可能先发 PJL 状态查询，等回应后才发作业
        std::vector<unsigned char> head;
        head.reserve(8192);
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

            DWORD written = 0;
            ::WriteFile(hFile, buffer.data(), (DWORD)bytesRead, &written, nullptr);
            if (head.size() < 8192)
            {
                size_t take = (std::min)((size_t)bytesRead, 8192 - head.size());
                head.insert(head.end(), buffer.begin(), buffer.begin() + take);
            }

            // 首段就是 PJL 查询：立刻回应，不能等连接关闭（驱动在等这条回复）
            if (totalBytes == 0 && LooksLikePjlQuery(head))
            {
                const std::string firstChunk(head.begin(), head.end());
                if (!IsRealPrintJob(firstChunk))
                {
                    const std::string response = BuildPjlResponse(firstChunk);
                    ::send(clientSocket, response.data(), (int)response.size(), 0);
                    Log("[PJL] 已回应状态查询，未当作作业打印: " + clientAddress);
                    ::CloseHandle(hFile);
                    ::DeleteFileW(tempFile.c_str());
                    RemoveActiveClient(clientSocketValue);
                    return;
                }
            }
            totalBytes += bytesRead;
        }

        ::CloseHandle(hFile);

        if (readFailed && totalBytes == 0)
        {
            // 驱动探测端口后直接复位连接：属于正常现象，不该当成错误刷日志
            int err = ::WSAGetLastError();
            Log("客户端 " + clientAddress + " 提前断开（错误 " + std::to_string(err) + "，未收到数据）");
            ::DeleteFileW(tempFile.c_str());
            RemoveActiveClient(clientSocketValue);
            return;
        }

        if (readFailed)
        {
            int err = ::WSAGetLastError();
            Log("处理客户端 " + clientAddress + " 中断: 接收数据失败 (错误 " + std::to_string(err) +
                ")，已收到 " + FormatSize(totalBytes) + "，放弃本作业");
            ::DeleteFileW(tempFile.c_str());
            RemoveActiveClient(clientSocketValue);
            return;
        }

        Log("接收数据: " + FormatSize(totalBytes) + " 来自 " + clientAddress);

        // RAW 直通打印
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
