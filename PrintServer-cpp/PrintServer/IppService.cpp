#include "pch.h"
#include "IppService.h"

#include "Config.h"
#include "Crypto.h"
#include "DriverPackager.h"
#include "IppMdns.h"
#include "IppPdf.h"
#include "IppRaster.h"
#include "LogHelper.h"
#include "Wmi.h"

#include <cmath>

#include <http.h>
#include <winspool.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <string.h>

#pragma comment(lib, "httpapi.lib")
#pragma comment(lib, "winspool.lib")

namespace ps
{

namespace
{

// ---------------------------------------------------------------------------
// 硬性上限（防止恶意/异常请求拖垮服务）
// ---------------------------------------------------------------------------
const int    kMaxConcurrentRequests = 16;
const size_t kHeadBufferLimit       = 1024 * 1024;      // 属性区最多缓存 1MB
const size_t kReadChunkSize         = 64 * 1024;
const size_t kJobRetentionMax       = 512;
const long long kMaxCopies          = 99;

// 支持的文档格式：默认是 PDL 直通（设备原生语言）；
// 具备 PDF 栅格化能力时（Phase B）额外支持 application/pdf 免驱打印。
const char* const kDefaultDocumentFormat = "application/octet-stream";
const char* const kPdfMimeType = "application/pdf";

bool IsPdfFormatName(const std::string& format)
{
    return _stricmp(format.c_str(), kPdfMimeType) == 0;
}

int ReadConfigInt(const char* key, int defaultValue)
{
    std::string text = Config::GetSetting(key, std::to_string(defaultValue));
    int value = atoi(text.c_str());
    return (value > 0) ? value : defaultValue;
}

bool ReadConfigBool(const char* key, bool defaultValue)
{
    std::string text = Config::GetSetting(key, defaultValue ? "true" : "false");
    std::string lower;
    for (char c : text)
        lower.push_back((char)tolower((unsigned char)c));
    return (lower == "true" || lower == "1");
}

std::string ToLowerCopy(const std::string& text)
{
    std::string out = text;
    for (char& c : out)
        c = (char)tolower((unsigned char)c);
    return out;
}

std::string TrimAscii(const std::string& text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' ||
                           text[begin] == '\r' || text[begin] == '\n'))
    {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n'))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

// 属性值首字节取文本（无值时返回默认值）
std::string FirstText(const Ipp::Request& request, const char* name, const std::string& def)
{
    return request.GetText(Ipp::Tag::OperationAttributes, name, def);
}

// MIME 类型 → IEEE 1284 命令集名（printer-device-id 的 CMD: 字段）。
// 注意 CMD 必须写命令集名（PDF/PCL/POSTSCRIPT…），不能写 MIME 类型，
// 否则 Windows 的 IPP 类驱动无法据 device-id 建队列。
std::string Ieee1284CommandSet(const std::string& format)
{
    if (_stricmp(format.c_str(), "application/pdf") == 0)
        return "PDF";
    if (_stricmp(format.c_str(), "application/postscript") == 0)
        return "POSTSCRIPT";
    if (_stricmp(format.c_str(), "application/vnd.hp-PCL") == 0)
        return "PCL";
    if (_stricmp(format.c_str(), "application/vnd.hp-PCLXL") == 0)
        return "PCLXL";
    if (_stricmp(format.c_str(), "image/pwg-raster") == 0 ||
        _stricmp(format.c_str(), "image/urf") == 0)
        return "PWGRaster";
    if (_stricmp(format.c_str(), "application/vnd.epson.escpage") == 0)
        return "ESCP";
    if (_stricmp(format.c_str(), "application/octet-stream") == 0)
        return std::string();   // 设备原生语言不确定，交由调用方兜底

    return std::string();
}

// 生成稳定的 printer-uuid：同一台机器 + 同一打印机名称 → 固定不变。
// 客户端据此避免重复创建打印机条目，因此必须稳定，不能随机。
std::string BuildPrinterUuid(const std::string& printerName)
{
    char hostName[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
    DWORD len = _countof(hostName);
    if (!::GetComputerNameA(hostName, &len))
        hostName[0] = '\0';

    std::vector<unsigned char> digest = Crypto::Sha256(std::string(hostName) + "|" + printerName);
    if (digest.size() < 16)
        return std::string();

    static const char* kHex = "0123456789abcdef";
    std::string out = "urn:uuid:";
    for (int i = 0; i < 16; ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out.push_back('-');
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }

    return out;
}

// 客户端提交了本实现无法兑现的作业模板属性时，应回 successful-ok-ignored-or-substituted
bool HasSubstitutedAttributes(const Ipp::Request& request, bool duplexSupported, bool colorSupported,
                              const std::string& defaultMedia, const std::string& defaultResolutionRaw)
{
    std::string sides = FirstText(request, "sides", "one-sided");
    if (!duplexSupported && _stricmp(sides.c_str(), "one-sided") != 0)
        return true;

    std::string colorMode = FirstText(request, "print-color-mode", "monochrome");
    if (!colorSupported && _stricmp(colorMode.c_str(), "monochrome") != 0)
        return true;

    std::string media = FirstText(request, "media", defaultMedia);
    if (_stricmp(media.c_str(), defaultMedia.c_str()) != 0)
        return true;

    long long orientation = 3;
    if (request.GetInt(Ipp::Tag::OperationAttributes, "orientation-requested", orientation))
    {
        if (orientation != 3)
            return true;
    }

    const Ipp::Attribute* resolution = request.Find(Ipp::Tag::OperationAttributes, "printer-resolution");
    if (resolution != nullptr && !resolution->values.empty())
    {
        if (resolution->values[0].text != defaultResolutionRaw)
            return true;
    }

    return false;
}

// 读取 copies 并夹到 1..kMaxCopies
int ReadCopies(const Ipp::Request& request)
{
    long long copies = 1;
    if (request.GetInt(Ipp::Tag::OperationAttributes, "copies", copies))
    {
        if (copies < 1)
            copies = 1;
        if (copies > kMaxCopies)
            copies = kMaxCopies;
    }
    return (int)copies;
}

} // namespace

// ===========================================================================
// 构造 / 析构
// ===========================================================================

IppServer::IppServer(const std::vector<PrinterConfig>& printers, int port)
    : m_printers(printers)
    , m_port(port > 0 ? port : 631)
    , m_running(false)
    , m_activeRequests(0)
    , m_queue(0)
    , m_nextJobId(1)
    , m_startTime((long long)::time(nullptr))
    , m_maxDocumentBytes(64u * 1024u * 1024u)
    , m_duplexSupported(false)
    , m_colorSupported(false)
    , m_pdfRenderingEnabled(false)
    , m_renderDpi(300)
    , m_mdnsEnabled(true)
    , m_pausedMask(0)
{
    NetworkHelper::EnsureWinsock();

    m_prefix = L"http://+:" + std::to_wstring(m_port) + L"/";

    int maxDocumentMb = ReadConfigInt("IppMaxDocumentMB", 64);
    if (maxDocumentMb < 1)
        maxDocumentMb = 1;
    m_maxDocumentBytes = (size_t)maxDocumentMb * 1024u * 1024u;

    m_duplexSupported = ReadConfigBool("IppDuplex", false);
    m_colorSupported = ReadConfigBool("IppColor", false);

    // Phase B：PDF 免驱渲染（需要 Win10 1809+，Win7/2008 无此能力时自动降级为 PDL 直通）
    m_renderDpi = ReadConfigInt("IppRenderDpi", 300);
    if (m_renderDpi < 72)
        m_renderDpi = 300;
    if (m_renderDpi > 1200)
        m_renderDpi = 1200;

    m_pdfRenderingEnabled = ReadConfigBool("IppPdfRendering", true) && IppPdf::IsAvailable();

    // mDNS 自动发现（让打印机出现在客户端"添加打印机"列表里）
    m_mdnsEnabled = ReadConfigBool("IppMdnsEnabled", true);

    {
        std::vector<std::string> addresses = NetworkHelper::GetAllNetworkAddresses();
        m_primaryAddress = addresses.empty() ? "localhost" : addresses[0];
    }

    for (size_t i = 0; i < m_printers.size(); ++i)
    {
        std::unique_ptr<PrinterRuntime> runtime(new PrinterRuntime());

        // 每台打印机可接受的 PDL 列表（逗号分隔）；默认只支持通用二进制
        std::string pdlText = Config::GetSetting(
            "Printer" + std::to_string(i) + "_IppPdl", kDefaultDocumentFormat);

        std::string current;
        for (size_t k = 0; k <= pdlText.size(); ++k)
        {
            if (k == pdlText.size() || pdlText[k] == ',')
            {
                std::string item = TrimAscii(current);
                if (!item.empty())
                    runtime->pdlFormats.push_back(item);
                current.clear();
            }
            else
            {
                current.push_back(pdlText[k]);
            }
        }

        if (runtime->pdlFormats.empty())
            runtime->pdlFormats.push_back(kDefaultDocumentFormat);

        // 驱动真实能力：彩色 / 双面 / 纸张 / 分辨率。
        // IPP 属性全部由它推导，避免"彩色机被当黑白、针式机被当 A4"。
        {
            IppRaster::PrinterCapabilities caps;
            std::string capsError;

            if (IppRaster::QueryPrinterCapabilities(m_printers[i].name, caps, capsError))
            {
                runtime->caps = caps;
                runtime->dpiX = caps.dpiX;
                runtime->dpiY = caps.dpiY;

                Log("打印机「" + m_printers[i].name + "」能力: " +
                    (caps.color ? "彩色" : "黑白") + ", " +
                    (caps.duplex ? "支持双面" : "单面") + ", " +
                    std::to_string(caps.dpiX) + "x" + std::to_string(caps.dpiY) + " dpi, " +
                    "纸张 " + std::to_string(caps.mediaKeywords.size()) + " 种" +
                    "（默认 " + std::to_string(caps.paperWidthMm) + "x" +
                    std::to_string(caps.paperHeightMm) + "mm）");
            }
            else
            {
                Log("打印机「" + m_printers[i].name + "」能力探测失败: " + capsError);
            }
        }

        // 缓存型号信息（只查一次，避免每次 Get-Printer-Attributes 都走 WMI）
        try
        {
            PrinterDriverInfo info = DriverPackager::GetPrinterDriverInfo(m_printers[i].name);
            runtime->manufacturer = info.manufacturer;
            if (!info.driverName.empty())
                runtime->makeAndModel = info.driverName;
            else
                runtime->makeAndModel = m_printers[i].name;
        }
        catch (...)
        {
            runtime->makeAndModel = m_printers[i].name;
        }

        // printer-device-id 必须是合法的 IEEE 1284 设备 ID 字符串：
        //   MFG 不能为空 —— Windows 的 IPP 类驱动会据它建打印队列，空值会导致
        //   "Windows 无法连接到打印机" 这类安装失败。
        std::string manufacturer = TrimAscii(runtime->manufacturer);
        if (manufacturer.empty())
        {
            // 退而求其次：取型号的第一个词（"Lenovo M7298W" → "Lenovo"）
            size_t space = runtime->makeAndModel.find(' ');
            manufacturer = (space == std::string::npos) ? runtime->makeAndModel
                                                        : runtime->makeAndModel.substr(0, space);
        }
        if (manufacturer.empty())
            manufacturer = "Generic";

        std::vector<std::string> commands;
        if (m_pdfRenderingEnabled)
            commands.push_back("PDF");

        for (size_t k = 0; k < runtime->pdlFormats.size(); ++k)
        {
            std::string mapped = Ieee1284CommandSet(runtime->pdlFormats[k]);
            if (!mapped.empty() &&
                std::find(commands.begin(), commands.end(), mapped) == commands.end())
            {
                commands.push_back(mapped);
            }
        }

        if (commands.empty())
            commands.push_back("PDF");   // 至少要有一个命令集，否则客户端无法识别

        std::string command;
        for (size_t k = 0; k < commands.size(); ++k)
        {
            if (k > 0)
                command += ",";
            command += commands[k];
        }

        runtime->deviceId = "MFG:" + manufacturer + ";MDL:" + runtime->makeAndModel +
                            ";CMD:" + command + ";";
        runtime->uuid = BuildPrinterUuid(m_printers[i].name);

        m_runtimes.push_back(std::move(runtime));
    }
}

IppServer::~IppServer()
{
    Stop();
}

void IppServer::Log(const std::string& message)
{
    std::string line = "[IPP] " + message;

    // 宿主（服务模式 / GUI）负责写文件与事件日志；这里再写一次会导致
    // 每条 IPP 日志重复两遍（1.4.2 修复）。没有 OnLog 时才自己落盘。
    if (OnLog)
    {
        OnLog(line);
        return;
    }

    Log::WriteLog(line);
}

// ===========================================================================
// 生命周期
// ===========================================================================

void IppServer::Start()
{
    if (m_running.load())
        return;

    // HTTP.sys 已由 HttpApiServer 初始化；这里再调一次是幂等的，
    // 但停止时不调用 HttpTerminate（交由 HttpApiServer 统一终结），
    // 避免提前终止进程级 HTTP 服务会话而影响控制 API。
    ::HttpInitialize(HTTPAPI_VERSION_1, HTTP_INITIALIZE_SERVER, nullptr);

    HANDLE queue = nullptr;
    if (::HttpCreateHttpHandle(&queue, 0) != NO_ERROR)
    {
        throw std::runtime_error("IPP: HttpCreateHttpHandle 失败");
    }

    if (::HttpAddUrl(queue, m_prefix.c_str(), nullptr) != NO_ERROR)
    {
        DWORD error = ::GetLastError();

        // 与 HttpApiServer 一致的降级策略：通配地址注册失败（端口被占用 /
        // 缺少 URL ACL）时退到本机回环，至少本机 IPP 客户端仍可使用；
        // 降级后不再做 mDNS 广播，避免把客户端引到一个连不上的地址。
        std::wstring fallback = L"http://localhost:" + std::to_wstring(m_port) + L"/";
        if (::HttpAddUrl(queue, fallback.c_str(), nullptr) == NO_ERROR)
        {
            m_prefix = fallback;
            m_mdnsEnabled = false;
            Log("IPP: 通配地址注册失败（错误 " + std::to_string(error) +
                "），已降级为 localhost，mDNS 广播关闭");
        }
        else
        {
            ::CloseHandle(queue);
            throw std::runtime_error("IPP: 注册监听前缀失败（端口 " + std::to_string(m_port) +
                                     "，错误 " + std::to_string(error) +
                                     "，可能已被其它程序占用）");
        }
    }

    m_queue = (uintptr_t)queue;
    m_running.store(true);

    StartWorkers();

    m_listenThread = std::thread([this]() { ListenLoop(); });

    Log("IPP 服务已启动，端口 " + std::to_string(m_port) + "，打印机 " +
        std::to_string(m_printers.size()) + " 台，PDF 免驱渲染=" +
        (m_pdfRenderingEnabled
             ? ("开启（按打印机分辨率渲染，上限 600 dpi）")
             : std::string("关闭（仅 PDL 直通）")));

    // ---- mDNS / DNS-SD 广播：让打印机自动出现在客户端「添加打印机」列表 ----
    // 实测结论：Windows 这一代可靠的是"发现式安装"，手输 URL 走的是旧 inetpp 路径，
    // 队列能建但打印会在本地失败，所以发现能力是 IPP 能否真正用起来的关键一环。
    if (m_mdnsEnabled && m_primaryAddress != "localhost" && !m_printers.empty())
    {
        std::vector<IppMdns::PrinterService> services;

        for (size_t i = 0; i < m_printers.size(); ++i)
        {
            IppMdns::PrinterService service;
            service.name = m_printers[i].name;
            service.ippPort = m_port;
            // rp 必须与 PrinterUriFor 的路径一致（同一套稳定槽位编号），
            // 否则客户端按发现结果拼出来的 URL 会 404 / 打到别的打印机。
            {
                const int token = m_printers[i].index;
                service.ippPath = (token <= 0) ? std::string("ipp/print")
                                               : ("ipp/print/" + std::to_string(token));
            }
            service.rawPort = m_printers[i].port;
            service.color = m_colorSupported;
            service.duplex = m_duplexSupported;

            if (i < m_runtimes.size() && m_runtimes[i] != nullptr)
            {
                // DNS-SD 的彩色/双面标记也取自驱动能力
                if (m_runtimes[i]->caps.valid)
                {
                    service.color = m_runtimes[i]->caps.color;
                    service.duplex = m_runtimes[i]->caps.duplex;
                }

                service.makeAndModel = m_runtimes[i]->makeAndModel;
                service.uuid = m_runtimes[i]->uuid;

                std::string pdl;
                if (m_pdfRenderingEnabled)
                    pdl = kPdfMimeType;

                for (size_t k = 0; k < m_runtimes[i]->pdlFormats.size(); ++k)
                {
                    if (!pdl.empty())
                        pdl += ",";
                    pdl += m_runtimes[i]->pdlFormats[k];
                }

                service.pdl = pdl.empty() ? std::string(kDefaultDocumentFormat) : pdl;
            }
            else
            {
                service.makeAndModel = m_printers[i].name;
                service.pdl = kDefaultDocumentFormat;
            }

            services.push_back(service);
        }

        char hostName[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
        DWORD hostLength = _countof(hostName);
        if (!::GetComputerNameA(hostName, &hostLength))
            hostName[0] = '\0';

        std::string mdnsError;
        std::string mdnsHost = (hostName[0] != '\0') ? std::string(hostName) : std::string("printserver");

        if (IppMdns::Start(mdnsHost, m_primaryAddress, services, mdnsError))
        {
            Log("mDNS 广播已启动: " + std::to_string(services.size()) +
                " 台打印机 (_ipp._tcp / _pdl-datastream._tcp)");
        }
        else
        {
            Log("mDNS 广播启动失败（不影响 IPP 与 RAW 本身）: " + mdnsError);
        }
    }
    else if (!m_mdnsEnabled)
    {
        Log("mDNS 广播已在配置中关闭 (IppMdnsEnabled=false)");
    }
}

void IppServer::Stop()
{
    // 先停 mDNS（独立线程，与请求处理无关），避免关机过程中还在发包
    IppMdns::Stop();

    if (!m_running.exchange(false))
    {
        StopWorkers();
        return;
    }

    HANDLE queue = (HANDLE)m_queue;
    if (queue != nullptr)
    {
        ::HttpRemoveUrl(queue, m_prefix.c_str());
        ::CloseHandle(queue);      // 唤醒阻塞中的 HttpReceiveHttpRequest
        m_queue = 0;
    }

    if (m_listenThread.joinable())
        m_listenThread.join();

    // 等待在途请求结束（限时，避免退出被长请求卡死）
    {
        std::unique_lock<std::mutex> lock(m_drainMutex);
        m_drainCondition.wait_for(lock, std::chrono::seconds(10),
                                  [this]() { return m_activeRequests.load() == 0; });
    }

    StopWorkers();
    Log("IPP 服务已停止");
}

void IppServer::StartWorkers()
{
    for (size_t i = 0; i < m_runtimes.size(); ++i)
    {
        m_runtimes[i]->stopRequested = false;
        m_runtimes[i]->worker = std::thread([this, i]() { WorkerLoop((int)i); });
    }
}

void IppServer::StopWorkers()
{
    for (size_t i = 0; i < m_runtimes.size(); ++i)
    {
        {
            std::lock_guard<std::mutex> guard(m_runtimes[i]->mutex);
            m_runtimes[i]->stopRequested = true;
            m_runtimes[i]->queue.clear();
        }
        m_runtimes[i]->condition.notify_all();

        if (m_runtimes[i]->worker.joinable())
            m_runtimes[i]->worker.join();
    }
}

// ===========================================================================
// 监听
// ===========================================================================

void IppServer::ListenLoop()
{
    std::vector<unsigned char> buffer(sizeof(HTTP_REQUEST) + 8192);

    while (m_running.load())
    {
        HANDLE queue = (HANDLE)m_queue;
        if (queue == nullptr)
            break;

        ULONG bytesReturned = 0;
        ULONG result = ::HttpReceiveHttpRequest(queue, HTTP_NULL_ID, 0,
                                               (PHTTP_REQUEST)buffer.data(),
                                               (ULONG)buffer.size(),
                                               &bytesReturned, nullptr);

        if (result == ERROR_MORE_DATA)
        {
            if (bytesReturned > buffer.size())
                buffer.resize(bytesReturned);
            continue;
        }

        if (result != NO_ERROR)
        {
            if (!m_running.load())
                break;
            ::Sleep(50);
            continue;
        }

        PHTTP_REQUEST request = (PHTTP_REQUEST)buffer.data();

        // 只处理 POST + application/ipp；其它请求立即给明确响应（不留悬空连接）
        std::string contentType;
        {
            const HTTP_KNOWN_HEADER& known = request->Headers.KnownHeaders[HttpHeaderContentType];
            if (known.pRawValue != nullptr && known.RawValueLength > 0)
                contentType.assign(known.pRawValue, known.RawValueLength);
        }

        std::string path = WideToUtf8(std::wstring(
            request->CookedUrl.pAbsPath,
            request->CookedUrl.AbsPathLength / sizeof(wchar_t)));

        std::string query;
        if (request->CookedUrl.pQueryString != nullptr && request->CookedUrl.QueryStringLength > 0)
        {
            query = WideToUtf8(std::wstring(request->CookedUrl.pQueryString,
                                            request->CookedUrl.QueryStringLength / sizeof(wchar_t)));
        }

        std::string clientIp = "未知";
        if (request->Address.pRemoteAddress != nullptr)
        {
            char text[64] = { 0 };
            if (request->Address.pRemoteAddress->sa_family == AF_INET)
            {
                sockaddr_in* a4 = (sockaddr_in*)request->Address.pRemoteAddress;
                if (::inet_ntop(AF_INET, &a4->sin_addr, text, sizeof(text)) != nullptr)
                    clientIp = text;
            }
            else if (request->Address.pRemoteAddress->sa_family == AF_INET6)
            {
                sockaddr_in6* a6 = (sockaddr_in6*)request->Address.pRemoteAddress;
                if (::inet_ntop(AF_INET6, &a6->sin6_addr, text, sizeof(text)) != nullptr)
                    clientIp = text;
            }
        }

        bool hasEntityBody = (request->Flags & HTTP_REQUEST_FLAG_MORE_ENTITY_BODY_EXISTS) != 0;
        uintptr_t requestId = (uintptr_t)request->RequestId;

        // 并发上限：超限时给出明确的 503，而不是让客户端一直等待
        if (m_activeRequests.load() >= kMaxConcurrentRequests)
        {
            static const char* kBusy = "Server busy";
            HTTP_RESPONSE response;
            memset(&response, 0, sizeof(response));
            response.StatusCode = 503;
            response.pReason = "Service Unavailable";
            response.ReasonLength = (USHORT)strlen(response.pReason);
            response.Headers.KnownHeaders[HttpHeaderContentType].pRawValue = "text/plain";
            response.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength = 10;

            HTTP_DATA_CHUNK chunk;
            memset(&chunk, 0, sizeof(chunk));
            chunk.DataChunkType = HttpDataChunkFromMemory;
            chunk.FromMemory.pBuffer = (PVOID)kBusy;
            chunk.FromMemory.BufferLength = (ULONG)strlen(kBusy);
            response.EntityChunkCount = 1;
            response.pEntityChunks = &chunk;

            ULONG sent = 0;
            ::HttpSendHttpResponse(queue, request->RequestId, 0, &response, nullptr, &sent,
                                   nullptr, 0, nullptr, nullptr);
            continue;
        }

        m_activeRequests.fetch_add(1);

        std::shared_ptr<IppServer> self = shared_from_this();
        std::thread([self, requestId, path, query, contentType, clientIp, hasEntityBody]() {
            try
            {
                self->HandleRequestThread(requestId, path, query, contentType, clientIp,
                                          hasEntityBody);
            }
            catch (...)
            {
                // 请求线程绝不允许异常逃逸（否则进程会终止）
            }

            self->m_activeRequests.fetch_sub(1);
            self->m_drainCondition.notify_all();
        }).detach();
    }
}

// ===========================================================================
// 请求处理
// ===========================================================================

namespace
{

bool SendHttpResponse(HANDLE queue, uintptr_t requestId, unsigned short statusCode,
                      const char* reason, const char* contentType,
                      const std::vector<unsigned char>& body)
{
    HTTP_RESPONSE response;
    memset(&response, 0, sizeof(response));
    response.StatusCode = statusCode;
    response.pReason = (char*)reason;
    response.ReasonLength = (USHORT)strlen(reason);
    response.Headers.KnownHeaders[HttpHeaderContentType].pRawValue = (char*)contentType;
    response.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength = (USHORT)strlen(contentType);

    HTTP_DATA_CHUNK chunk;
    memset(&chunk, 0, sizeof(chunk));

    if (!body.empty())
    {
        chunk.DataChunkType = HttpDataChunkFromMemory;
        chunk.FromMemory.pBuffer = (PVOID)body.data();
        chunk.FromMemory.BufferLength = (ULONG)body.size();
        response.EntityChunkCount = 1;
        response.pEntityChunks = &chunk;
    }

    ULONG sent = 0;
    ULONG result = ::HttpSendHttpResponse(queue, (HTTP_REQUEST_ID)requestId, 0, &response, nullptr,
                                         &sent, nullptr, 0, nullptr, nullptr);
    return result == NO_ERROR;
}

// 把文档边收边落盘；headOut 保存前 kHeadBufferLimit 字节用于解析属性
bool ReadEntityBodyToFile(HANDLE queue, uintptr_t requestId, const std::wstring& filePath,
                          std::string& headOut, size_t& totalBytes, bool& bodyComplete,
                          int& errorStatus)
{
    totalBytes = 0;
    bodyComplete = false;
    errorStatus = 0;
    headOut.clear();

    HANDLE file = ::CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        errorStatus = Ipp::Status::ServerErrorInternalError;
        return false;
    }

    std::vector<unsigned char> buffer(kReadChunkSize);
    bool ok = true;

    for (;;)
    {
        ULONG read = 0;
        ULONG result = ::HttpReceiveRequestEntityBody(queue, (HTTP_REQUEST_ID)requestId, 0,
                                                     buffer.data(), (ULONG)buffer.size(),
                                                     &read, nullptr);
        if (result == ERROR_HANDLE_EOF)
        {
            bodyComplete = true;
            break;
        }

        if (result != NO_ERROR || read == 0)
        {
            ok = false;
            errorStatus = Ipp::Status::ClientErrorBadRequest;
            break;
        }

        totalBytes += read;

        DWORD written = 0;
        if (!::WriteFile(file, buffer.data(), read, &written, nullptr) || written != read)
        {
            ok = false;
            errorStatus = Ipp::Status::ServerErrorInternalError;
            break;
        }

        if (headOut.size() < kHeadBufferLimit)
        {
            size_t room = kHeadBufferLimit - headOut.size();
            size_t take = (read < room) ? read : room;
            headOut.append((const char*)buffer.data(), take);
        }
    }

    ::CloseHandle(file);

    if (!ok)
        return false;

    return true;
}

std::string StatusReason(unsigned short status)
{
    switch (status)
    {
    case Ipp::Status::Ok: return "ok";
    case Ipp::Status::OkIgnoredOrSubstituted: return "ok-ignored-or-substituted-attributes";
    case Ipp::Status::ClientErrorBadRequest: return "client-error-bad-request";
    case Ipp::Status::ClientErrorNotFound: return "client-error-not-found";
    case Ipp::Status::ClientErrorNotPossible: return "client-error-not-possible";
    case Ipp::Status::ClientErrorDocumentFormatNotSupported: return "client-error-document-format-not-supported";
    case Ipp::Status::ClientErrorCharsetNotSupported: return "client-error-charset-not-supported";
    case Ipp::Status::ClientErrorRequestEntityTooLarge: return "client-error-request-entity-too-large";
    case Ipp::Status::ServerErrorInternalError: return "server-error-internal-error";
    case Ipp::Status::ServerErrorOperationNotSupported: return "server-error-operation-not-supported";
    case Ipp::Status::ServerErrorBusy: return "server-error-busy";
    case Ipp::Status::ServerErrorNotAcceptingJobs: return "server-error-not-accepting-jobs";
    default: return "status-0x" + std::to_string(status);
    }
}

} // namespace

void IppServer::HandleRequestThread(uintptr_t requestId, const std::string& path,
                                    const std::string& query, const std::string& contentType,
                                    const std::string& clientIp, bool hasEntityBody)
{
    HANDLE queue = (HANDLE)m_queue;
    if (queue == nullptr)
        return;

    std::string lowerContentType = ToLowerCopy(contentType);
    bool isIpp = (lowerContentType.find("application/ipp") != std::string::npos);
    bool isPrintPath = (ToLowerCopy(path).find("/ipp") == 0) ||
                       (ToLowerCopy(path).find("/printers") == 0);

    // 记录每个进入的请求：诊断客户端兼容问题时，"它请求了哪个路径" 是最关键的信息
    // （Windows 添加 IPP 打印机时会尝试多个 URL 变体，逐个探测）。
    Log("请求 " + path + query + "  Content-Type=" +
        (contentType.empty() ? std::string("(空)") : contentType) + "  实体体=" +
        (hasEntityBody ? "有" : "无") + "  来自 " + clientIp + (isIpp ? "" : "  [非IPP]"));

    if (!isIpp)
    {
        // 浏览器/探测请求：给一个人畜无害的说明，避免连接悬空
        std::string text = "PrintServer IPP endpoint. Use IPP over HTTP (Content-Type: application/ipp).\r\n";
        std::vector<unsigned char> body(text.begin(), text.end());
        if (isPrintPath)
            SendHttpResponse(queue, requestId, 200, "OK", "text/plain; charset=utf-8", body);
        else
            SendHttpResponse(queue, requestId, 404, "Not Found", "text/plain; charset=utf-8", body);
        return;
    }

    if (!hasEntityBody)
    {
        Ipp::ResponseBuilder builder;
        builder.Begin(1, 1, Ipp::Status::ClientErrorBadRequest, 0);
        builder.BeginGroup(Ipp::Tag::OperationAttributes);
        builder.AddCharset("attributes-charset", "utf-8");
        builder.AddNaturalLanguage("attributes-natural-language", "en-us");
        builder.EndAttributes();
        SendHttpResponse(queue, requestId, 200, "OK", "application/ipp", builder.Bytes());
        return;
    }

    std::wstring tempPath = MakeJobTempPath();
    std::string head;
    size_t totalBytes = 0;
    bool bodyComplete = false;
    int errorStatus = 0;

    bool readOk = ReadEntityBodyToFile(queue, requestId, tempPath, head, totalBytes,
                                       bodyComplete, errorStatus);

    if (!readOk || totalBytes > m_maxDocumentBytes)
    {
        ::DeleteFileW(tempPath.c_str());

        unsigned short status = readOk ? Ipp::Status::ClientErrorRequestEntityTooLarge : errorStatus;

        // 尽量从已收到的头部回填版本与 request-id，保证客户端能对上号
        unsigned char major = 1;
        unsigned char minor = 1;
        unsigned long requestIdValue = 0;
        if (head.size() >= 8)
        {
            major = (unsigned char)head[0];
            minor = (unsigned char)head[1];
            if (!Ipp::IsSupportedVersion(major, minor))
            {
                major = 1;
                minor = 1;
            }
            requestIdValue = ((unsigned long)(unsigned char)head[4] << 24) |
                             ((unsigned long)(unsigned char)head[5] << 16) |
                             ((unsigned long)(unsigned char)head[6] << 8) |
                             (unsigned long)(unsigned char)head[7];
        }

        Ipp::ResponseBuilder builder;
        builder.Begin(major, minor, status, requestIdValue);
        builder.BeginGroup(Ipp::Tag::OperationAttributes);
        builder.AddCharset("attributes-charset", "utf-8");
        builder.AddNaturalLanguage("attributes-natural-language", "en-us");

        if (status == Ipp::Status::ClientErrorRequestEntityTooLarge)
        {
            long long maxMb = (long long)(m_maxDocumentBytes / (1024 * 1024));
            builder.AddKeyword("status-message", "request entity too large (limit " +
                                                     std::to_string(maxMb) + " MB)");
        }

        builder.EndAttributes();

        Log("拒绝请求: " + StatusReason(status) + " (body " + std::to_string(totalBytes) +
            " 字节) 来自 " + clientIp);
        SendHttpResponse(queue, requestId, 200, "OK", "application/ipp", builder.Bytes());
        return;
    }

    // 解析 IPP
    Ipp::Request ippRequest;
    bool parsed = Ipp::ParseRequest((const unsigned char*)head.data(), head.size(), ippRequest);

    std::vector<unsigned char> responseBytes;
    int statusCode = Ipp::Status::Ok;
    bool bodyClaimedByJob = false;

    if (!parsed || !ippRequest.parseError.empty())
    {
        Ipp::ResponseBuilder builder;
        builder.Begin(1, 1, Ipp::Status::ClientErrorBadRequest, 0);
        builder.BeginGroup(Ipp::Tag::OperationAttributes);
        builder.AddCharset("attributes-charset", "utf-8");
        builder.AddNaturalLanguage("attributes-natural-language", "en-us");
        builder.EndAttributes();
        responseBytes = builder.Bytes();

        Log("解析失败(" + ippRequest.parseError + ") 来自 " + clientIp);
    }
    else
    {
        int major = ippRequest.majorVersion;
        int minor = ippRequest.minorVersion;
        Ipp::ClampVersion(major, minor);
        ippRequest.majorVersion = major;
        ippRequest.minorVersion = minor;

        // head 只缓存了前 1MB：文档是否存在要以整包大小判断
        ippRequest.hasDocument = (totalBytes > ippRequest.documentOffset);

        try
        {
            DispatchOperation(ippRequest, clientIp, tempPath, totalBytes, bodyComplete,
                              bodyClaimedByJob, responseBytes, statusCode);
        }
        catch (const std::exception& ex)
        {
            Log("处理异常: " + std::string(ex.what()));
            responseBytes.clear();
            statusCode = Ipp::Status::ServerErrorInternalError;
        }
        catch (...)
        {
            Log("处理异常: 未知错误");
            responseBytes.clear();
            statusCode = Ipp::Status::ServerErrorInternalError;
        }

        if (responseBytes.empty())
        {
            // 兜底：任何情况下都必须回一个合法 IPP 体，否则客户端会等到超时
            Ipp::ResponseBuilder builder;
            builder.Begin((unsigned char)ippRequest.majorVersion,
                          (unsigned char)ippRequest.minorVersion,
                          (unsigned short)statusCode, ippRequest.requestId);
            builder.BeginGroup(Ipp::Tag::OperationAttributes);
            builder.AddCharset("attributes-charset", "utf-8");
            builder.AddNaturalLanguage("attributes-natural-language", "en-us");
            builder.AddKeyword("status-message", StatusReason((unsigned short)statusCode));
            builder.EndAttributes();
            responseBytes = builder.Bytes();
        }
    }

    // 作业已接管文档文件时不能删除
    if (!bodyClaimedByJob)
        ::DeleteFileW(tempPath.c_str());

    SendHttpResponse(queue, requestId, 200, "OK", "application/ipp", responseBytes);
}

// ===========================================================================
// 操作分发
// ===========================================================================

void IppServer::DispatchOperation(const Ipp::Request& request, const std::string& clientIp,
                                  const std::wstring& bodyFilePath, size_t totalBodyBytes,
                                  bool /*bodyComplete*/, bool& bodyClaimedOut,
                                  std::vector<unsigned char>& responseBytes, int& ippStatusOut)
{
    bodyClaimedOut = false;
    Ipp::ResponseBuilder builder;
    builder.Begin((unsigned char)request.majorVersion, (unsigned char)request.minorVersion,
                  Ipp::Status::Ok, request.requestId);

    std::string charset = FirstText(request, "attributes-charset", "utf-8");
    std::string lowerCharset = ToLowerCopy(TrimAscii(charset));
    if (lowerCharset != "utf-8" && lowerCharset != "us-ascii")
    {
        builder.Begin(1, 1, Ipp::Status::ClientErrorCharsetNotSupported, request.requestId);
        builder.BeginGroup(Ipp::Tag::OperationAttributes);
        builder.AddCharset("attributes-charset", "utf-8");
        builder.AddNaturalLanguage("attributes-natural-language", "en-us");
        builder.EndAttributes();
        responseBytes = builder.Bytes();
        ippStatusOut = Ipp::Status::ClientErrorCharsetNotSupported;
        return;
    }

    int printerIndex = ResolvePrinterIndex(FirstText(request, "printer-uri", ""));
    if (printerIndex < 0 || printerIndex >= (int)m_printers.size())
    {
        Ipp::ResponseBuilder errorBuilder;
        errorBuilder.Begin((unsigned char)request.majorVersion, (unsigned char)request.minorVersion,
                           Ipp::Status::ClientErrorNotFound, request.requestId);
        errorBuilder.BeginGroup(Ipp::Tag::OperationAttributes);
        errorBuilder.AddCharset("attributes-charset", "utf-8");
        errorBuilder.AddNaturalLanguage("attributes-natural-language", "en-us");
        errorBuilder.AddKeyword("status-message", "printer not found");
        errorBuilder.EndAttributes();
        responseBytes = errorBuilder.Bytes();
        ippStatusOut = Ipp::Status::ClientErrorNotFound;
        return;
    }

    switch (request.operationId)
    {
    case Ipp::Op::GetPrinterAttributes:
        HandleGetPrinterAttributes(request, printerIndex, builder);
        break;
    case Ipp::Op::ValidateJob:
        HandleValidateJob(request, printerIndex, builder);
        break;
    case Ipp::Op::PrintJob:
        HandlePrintJob(request, printerIndex, bodyFilePath, totalBodyBytes, bodyClaimedOut, builder);
        break;
    case Ipp::Op::CreateJob:
        HandleCreateJob(request, printerIndex, builder);
        break;
    case Ipp::Op::SendDocument:
        HandleSendDocument(request, printerIndex, bodyFilePath, totalBodyBytes, bodyClaimedOut,
                           builder);
        break;
    case Ipp::Op::GetJobs:
        HandleGetJobs(request, printerIndex, builder);
        break;
    case Ipp::Op::GetJobAttributes:
        HandleGetJobAttributes(request, printerIndex, builder);
        break;
    case Ipp::Op::CancelJob:
        HandleCancelJob(request, printerIndex, builder);
        break;
    case Ipp::Op::CloseJob:
        HandleCloseJob(request, printerIndex, builder);
        break;
    case Ipp::Op::HoldJob:
        HandleHoldJob(request, printerIndex, true, builder);
        break;
    case Ipp::Op::ReleaseJob:
        HandleHoldJob(request, printerIndex, false, builder);
        break;
    case Ipp::Op::RestartJob:
        HandleRestartJob(request, printerIndex, builder);
        break;
    case Ipp::Op::PurgeJobs:
        HandlePurgeJobs(request, printerIndex, builder);
        break;
    case Ipp::Op::IdentifyPrinter:
        HandleIdentifyPrinter(request, printerIndex, builder);
        break;
    case Ipp::Op::PausePrinter:
        HandlePauseResume(request, printerIndex, true, builder);
        break;
    case Ipp::Op::ResumePrinter:
        HandlePauseResume(request, printerIndex, false, builder);
        break;
    default:
        {
            Ipp::ResponseBuilder errorBuilder;
            errorBuilder.Begin((unsigned char)request.majorVersion,
                               (unsigned char)request.minorVersion,
                               Ipp::Status::ServerErrorOperationNotSupported, request.requestId);
            errorBuilder.BeginGroup(Ipp::Tag::OperationAttributes);
            errorBuilder.AddCharset("attributes-charset", "utf-8");
            errorBuilder.AddNaturalLanguage("attributes-natural-language", "en-us");
            errorBuilder.AddKeyword("status-message", "operation not supported");
            errorBuilder.EndAttributes();
            responseBytes = errorBuilder.Bytes();
        }

        {
            char opText[16] = { 0 };
            _snprintf_s(opText, sizeof(opText), _TRUNCATE, "0x%04X", request.operationId);
            ippStatusOut = Ipp::Status::ServerErrorOperationNotSupported;
            Log("不支持的操作: " + std::string(opText));
        }
        return;
    }

    builder.EndAttributes();
    responseBytes = builder.Bytes();
    // 用 builder 的真实状态：handler 内部可能已替换为错误响应，
    // 这里若写死 Ok，日志就会把失败记录成成功（1.4.2 修复）。
    ippStatusOut = builder.Status();

    {
        char opText[16] = { 0 };
        char statusText[16] = { 0 };
        _snprintf_s(opText, sizeof(opText), _TRUNCATE, "0x%04X", request.operationId);
        _snprintf_s(statusText, sizeof(statusText), _TRUNCATE, "0x%04X", (unsigned)ippStatusOut);
        Log(std::string("操作 ") + opText + " 打印机#" + std::to_string(printerIndex) + " -> " +
            statusText + " 返回 " + std::to_string(responseBytes.size()) + " 字节 来自 " + clientIp);
    }
}

// ===========================================================================
// 属性输出
// ===========================================================================

void IppServer::AddOperationAttributes(Ipp::ResponseBuilder& builder) const
{
    builder.AddCharset("attributes-charset", "utf-8");
    builder.AddNaturalLanguage("attributes-natural-language", "en-us");
}

void IppServer::AddPrinterAttributes(Ipp::ResponseBuilder& builder, int printerIndex) const
{
    const PrinterRuntime& runtime = *m_runtimes[printerIndex];
    const PrinterConfig& config = m_printers[printerIndex];

    bool paused = printerIndex >= 0 && printerIndex < 64 &&
                  ((m_pausedMask.load() >> printerIndex) & 1ULL) != 0;
    int state = paused ? 5 : PrinterState(printerIndex);

    std::vector<std::string> addresses = NetworkHelper::GetAllNetworkAddresses();

    builder.BeginGroup(Ipp::Tag::PrinterAttributes);

    for (size_t i = 0; i < addresses.size(); ++i)
        builder.AddUri("printer-uri-supported", PrinterUriFor(printerIndex, addresses[i]));

    builder.AddKeyword("uri-authentication-supported", "none");
    builder.AddKeyword("uri-security-supported", "none");
    // printer-name 是"显示名"（nameWithoutLanguage），标准要求原样返回：
    // 客户端（含 Windows 建队列）会直接拿它当打印机名，做 _ 替换会让名字变样。
    builder.AddName("printer-name", config.name);
    builder.AddText("printer-info", config.name);

    // printer-location / printer-state-message 都必须是非空 text
    // （空字符串在 RFC 8010 里是非法编码，会被 Windows 的 IPP 解析器拒绝）
    {
        char hostName[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
        DWORD hostLength = _countof(hostName);
        if (!::GetComputerNameA(hostName, &hostLength))
            hostName[0] = '\0';

        builder.AddText("printer-location",
                        hostName[0] != '\0' ? std::string(hostName) : std::string("PrintServer"));
    }

    builder.AddText("printer-make-and-model", runtime.makeAndModel);

    // RFC 8011 把 printer-more-info 列为必备；Windows 的 IPP 类驱动也会读它。
    // 复用 printer-uri 的路径，只把 ipp:// 换成 http://，避免两处路径写法不一致。
    {
        std::string moreInfoUri = PrinterUriFor(printerIndex, m_primaryAddress);
        if (moreInfoUri.rfind("ipp://", 0) == 0)
            moreInfoUri.replace(0, 6, "http://");
        builder.AddUri("printer-more-info", moreInfoUri);
    }
    builder.AddText("printer-device-id", runtime.deviceId);
    if (!runtime.uuid.empty())
        builder.AddUri("printer-uuid", runtime.uuid);

    builder.AddEnum("printer-state", state);
    builder.AddKeyword("printer-state-reasons", paused ? "paused" : "none");
    builder.AddText("printer-state-message", paused ? "Printer paused" : "Ready to print");
    builder.AddBool("printer-is-accepting-jobs", !paused);
    builder.AddInt("queued-job-count", QueuedCount(printerIndex));
    builder.AddInt("printer-up-time", UpTime());
    builder.AddDateTime("printer-current-time", (time_t)::time(nullptr));

    builder.AddKeyword("ipp-versions-supported", "1.0");
    builder.AddKeyword("ipp-versions-supported", "1.1");
    builder.AddKeyword("ipp-versions-supported", "2.0");

    builder.AddEnum("operations-supported", Ipp::Op::PrintJob);
    builder.AddEnum("operations-supported", Ipp::Op::ValidateJob);
    builder.AddEnum("operations-supported", Ipp::Op::CreateJob);
    builder.AddEnum("operations-supported", Ipp::Op::SendDocument);
    builder.AddEnum("operations-supported", Ipp::Op::CancelJob);
    builder.AddEnum("operations-supported", Ipp::Op::GetJobAttributes);
    builder.AddEnum("operations-supported", Ipp::Op::GetJobs);
    builder.AddEnum("operations-supported", Ipp::Op::GetPrinterAttributes);
    builder.AddEnum("operations-supported", Ipp::Op::HoldJob);
    builder.AddEnum("operations-supported", Ipp::Op::ReleaseJob);
    builder.AddEnum("operations-supported", Ipp::Op::RestartJob);
    builder.AddEnum("operations-supported", Ipp::Op::PurgeJobs);
    builder.AddEnum("operations-supported", Ipp::Op::IdentifyPrinter);
    builder.AddEnum("operations-supported", Ipp::Op::PausePrinter);
    builder.AddEnum("operations-supported", Ipp::Op::ResumePrinter);
    builder.AddEnum("operations-supported", Ipp::Op::CloseJob);

    builder.AddCharset("charset-configured", "utf-8");
    builder.AddCharset("charset-supported", "utf-8");
    builder.AddCharset("charset-supported", "us-ascii");
    builder.AddNaturalLanguage("natural-language-configured", "en-us");
    builder.AddNaturalLanguage("generated-natural-language-supported", "en-us");

    // PDF 放在最前：免驱客户端（Windows IPP Class Driver / AirPrint / Mopria）优先选它
    if (m_pdfRenderingEnabled)
        builder.AddMimeType("document-format-supported", kPdfMimeType);

    for (size_t i = 0; i < runtime.pdlFormats.size(); ++i)
        builder.AddMimeType("document-format-supported", runtime.pdlFormats[i]);

    // 默认格式：具备 PDF 渲染能力时用 application/pdf（免驱客户端的通用格式），
    // 否则退回 PDL 直通的 octet-stream。
    // 注意：这个值必须在 document-format-supported 之内（上面已保证）。
    builder.AddMimeType("document-format-default",
                        m_pdfRenderingEnabled ? kPdfMimeType
                                              : (runtime.pdlFormats.empty() ? kDefaultDocumentFormat
                                                                            : runtime.pdlFormats[0]));

    // 能力判定：优先用驱动探测结果，探测失败才回退配置项
    const bool supportsColor = runtime.caps.valid ? runtime.caps.color : m_colorSupported;
    const bool supportsDuplex = runtime.caps.valid ? runtime.caps.duplex : m_duplexSupported;

    builder.AddKeyword("compression-supported", "none");
    builder.AddBool("multiple-document-jobs-supported", false);
    builder.AddBool("color-supported", supportsColor);
    builder.AddKeyword("pdl-override-supported", "attempted");
    builder.AddKeyword("printer-kind", "document");

    builder.AddKeyword("job-creation-attributes-supported", "copies");
    builder.AddKeyword("job-creation-attributes-supported", "document-format");
    builder.AddKeyword("job-creation-attributes-supported", "ipp-attribute-fidelity");
    builder.AddKeyword("job-creation-attributes-supported", "job-name");
    builder.AddKeyword("job-creation-attributes-supported", "job-priority");
    builder.AddKeyword("job-creation-attributes-supported", "media");
    builder.AddKeyword("job-creation-attributes-supported", "multiple-document-handling");
    builder.AddKeyword("job-creation-attributes-supported", "orientation-requested");
    builder.AddKeyword("job-creation-attributes-supported", "print-color-mode");
    builder.AddKeyword("job-creation-attributes-supported", "printer-resolution");
    builder.AddKeyword("job-creation-attributes-supported", "sides");

    builder.AddInt("copies-default", 1);
    builder.AddRange("copies-supported", 1, kMaxCopies);

    builder.AddEnum("finishings-default", 3);      // 3 = none
    builder.AddEnum("finishings-supported", 3);

    builder.AddKeyword("job-sheets-default", "none");
    builder.AddKeyword("job-sheets-supported", "none");

    // 纸张列表来自驱动（DC_PAPERS/DC_PAPERSIZE）：
    // 针式机的连续纸、彩机的大幅面都会在这里体现；探测失败才回退常见尺寸。
    if (runtime.caps.valid && !runtime.caps.mediaKeywords.empty())
    {
        const std::string& defaultMedia = runtime.caps.mediaKeywords.front();

        builder.AddKeyword("media-default", defaultMedia);
        builder.AddKeyword("media-ready", defaultMedia);

        for (size_t i = 0; i < runtime.caps.mediaKeywords.size(); ++i)
            builder.AddKeyword("media-supported", runtime.caps.mediaKeywords[i]);
    }
    else
    {
        builder.AddKeyword("media-default", "iso_a4_210x297mm");
        builder.AddKeyword("media-ready", "iso_a4_210x297mm");
        builder.AddKeyword("media-supported", "iso_a4_210x297mm");
        builder.AddKeyword("media-supported", "iso_a5_148x210mm");
        builder.AddKeyword("media-supported", "iso_a3_297x420mm");
        builder.AddKeyword("media-supported", "jis_b5_182x257mm");
        builder.AddKeyword("media-supported", "na_letter_8.5x11in");
        builder.AddKeyword("media-supported", "na_legal_8.5x14in");
    }

    builder.AddEnum("orientation-requested-default", 3);
    builder.AddEnum("orientation-requested-supported", 3);
    builder.AddEnum("orientation-requested-supported", 4);
    builder.AddEnum("orientation-requested-supported", 5);
    builder.AddEnum("orientation-requested-supported", 6);

    // 彩色模式：黑白机只宣告 monochrome；彩色机追加 color 并把默认值设为 color
    builder.AddKeyword("print-color-mode-default", supportsColor ? "color" : "monochrome");
    builder.AddKeyword("print-color-mode-supported", "monochrome");
    if (supportsColor)
        builder.AddKeyword("print-color-mode-supported", "color");

    const int defaultResX = (runtime.dpiX > 0) ? runtime.dpiX : 600;
    const int defaultResY = (runtime.dpiY > 0) ? runtime.dpiY : 600;

    builder.AddResolution("printer-resolution-default", defaultResX, defaultResY, 3);
    builder.AddResolution("printer-resolution-supported", defaultResX, defaultResY, 3);

    builder.AddKeyword("sides-default", "one-sided");
    builder.AddKeyword("sides-supported", "one-sided");
    if (supportsDuplex)
    {
        builder.AddKeyword("sides-default", "two-sided-long-edge");
        builder.AddKeyword("sides-supported", "two-sided-long-edge");
        builder.AddKeyword("sides-supported", "two-sided-short-edge");
    }

    // IPP Everywhere / Mopria 要求这两组；Windows 的 IPP 类驱动也会读
    builder.AddEnum("print-quality-default", 4);        // 4 = normal
    builder.AddEnum("print-quality-supported", 3);      // 3 = draft
    builder.AddEnum("print-quality-supported", 4);
    builder.AddEnum("print-quality-supported", 5);      // 5 = high

    builder.AddKeyword("output-bin-default", "face-down");
    builder.AddKeyword("output-bin-supported", "face-down");

    builder.AddKeyword("multiple-document-handling-default", "separate-documents-uncollated-copies");
    builder.AddKeyword("multiple-document-handling-supported", "separate-documents-uncollated-copies");

    builder.AddKeyword("identify-actions-default", "display");
    builder.AddKeyword("identify-actions-supported", "display");

    // ==== RFC 8011 / IPP Everywhere 常用属性（1.4.2 补齐）====
    builder.AddKeyword("which-jobs-supported", "completed");
    builder.AddKeyword("which-jobs-supported", "not-completed");
    builder.AddKeyword("which-jobs-supported", "all");

    builder.AddKeyword("job-hold-until-default", "no-hold");
    builder.AddKeyword("job-hold-until-supported", "no-hold");

    builder.AddInt("job-priority-default", 50);
    builder.AddRange("job-priority-supported", 1, 100);

    builder.AddRange("job-k-octets-supported", 0, (long long)(m_maxDocumentBytes / 1024));

    builder.AddInt("multiple-operation-time-out", 60);

    // DNS-SD 实例名（与 mDNS 广播的实例名保持一致，便于按名字定位）
    builder.AddName("printer-dns-sd-name", config.name);
}

void IppServer::AddJobAttributes(Ipp::ResponseBuilder& builder, const IppJobPtr& job) const
{
    builder.BeginGroup(Ipp::Tag::JobAttributes);

    builder.AddUri("job-uri", JobUriFor(job));
    builder.AddInt("job-id", job->id);
    builder.AddEnum("job-state", job->state);
    builder.AddKeyword("job-state-reasons", job->stateReasons);
    builder.AddText("job-state-message",
                    job->state == IppJobCompleted ? "Job completed" : "Job in progress");
    builder.AddName("job-name", job->name);
    builder.AddName("job-originating-user-name", job->userName);
    builder.AddUri("job-printer-uri", PrinterUriFor(job->printerIndex, m_primaryAddress));
    builder.AddKeyword("job-printer-state-reasons", "none");
    builder.AddInt("time-at-creation", job->createdUpTime);
    builder.AddInt("time-at-processing", job->processingUpTime);
    builder.AddInt("time-at-completed", job->completedUpTime);
    builder.AddInt("job-k-octets", (long long)(job->documentBytes / 1024));
    builder.AddInt("job-impressions-completed", job->state == IppJobCompleted ? 1 : 0);
    builder.AddMimeType("document-format", job->documentFormat);
    builder.AddInt("job-priority", 50);
}

// ===========================================================================
// 操作实现
// ===========================================================================

void IppServer::HandleGetPrinterAttributes(const Ipp::Request& request, int printerIndex,
                                           Ipp::ResponseBuilder& builder)
{
    builder.BeginGroup(Ipp::Tag::OperationAttributes);
    AddOperationAttributes(builder);

    // requested-attributes 过滤（RFC 8011 §4.2.11）：
    // 客户端没要的属性不再返回；但身份/状态类必备属性始终返回，
    // 否则部分客户端（含 Windows IPP Class Driver）会因为读不到状态而拒绝添加。
    const std::vector<std::string> requested =
        request.GetTextList(Ipp::Tag::OperationAttributes, "requested-attributes");

    if (!requested.empty() && !request.WantsAllShown())
    {
        static const char* kAlwaysShown[] = {
            "printer-uri-supported", "uri-authentication-supported", "uri-security-supported",
            "printer-name", "printer-info", "printer-make-and-model", "printer-location",
            "printer-state", "printer-state-reasons", "printer-state-message",
            "printer-is-accepting-jobs", "queued-job-count", "printer-up-time",
            "printer-current-time", "ipp-versions-supported", "operations-supported",
            "charset-configured", "charset-supported",
            "natural-language-configured", "generated-natural-language-supported",
            "document-format-supported", "document-format-default",
        };

        builder.SetAttributeFilter([&request](const char* name) -> bool {
            for (size_t i = 0; i < _countof(kAlwaysShown); ++i)
            {
                if (_stricmp(name, kAlwaysShown[i]) == 0)
                    return true;
            }
            return request.Requested(Ipp::Tag::OperationAttributes, name);
        });
    }

    AddPrinterAttributes(builder, printerIndex);

    builder.SetAttributeFilter(nullptr);
}

void IppServer::HandleValidateJob(const Ipp::Request& request, int printerIndex,
                                  Ipp::ResponseBuilder& builder)
{
    builder.BeginGroup(Ipp::Tag::OperationAttributes);
    AddOperationAttributes(builder);

    std::string format = FirstText(request, "document-format", kDefaultDocumentFormat);
    if (!IsFormatSupported(printerIndex, format))
    {
        Ipp::ResponseBuilder errorBuilder;
        errorBuilder.Begin((unsigned char)request.majorVersion, (unsigned char)request.minorVersion,
                           Ipp::Status::ClientErrorDocumentFormatNotSupported, request.requestId);
        errorBuilder.BeginGroup(Ipp::Tag::OperationAttributes);
        errorBuilder.AddCharset("attributes-charset", "utf-8");
        errorBuilder.AddNaturalLanguage("attributes-natural-language", "en-us");
        errorBuilder.AddMimeType("document-format-supported", kDefaultDocumentFormat);
        errorBuilder.AddKeyword("status-message", "unsupported document-format: " + format);
        errorBuilder.EndAttributes();

        builder = errorBuilder;
        return;
    }
}

void IppServer::HandlePrintJob(const Ipp::Request& request, int printerIndex,
                               const std::wstring& bodyFilePath, size_t totalBodyBytes,
                               bool& bodyClaimed, Ipp::ResponseBuilder& builder)
{
    builder.BeginGroup(Ipp::Tag::OperationAttributes);
    AddOperationAttributes(builder);

    std::string format = FirstText(request, "document-format", kDefaultDocumentFormat);
    if (!IsFormatSupported(printerIndex, format))
    {
        Ipp::ResponseBuilder errorBuilder;
        errorBuilder.Begin((unsigned char)request.majorVersion, (unsigned char)request.minorVersion,
                           Ipp::Status::ClientErrorDocumentFormatNotSupported, request.requestId);
        errorBuilder.BeginGroup(Ipp::Tag::OperationAttributes);
        errorBuilder.AddCharset("attributes-charset", "utf-8");
        errorBuilder.AddNaturalLanguage("attributes-natural-language", "en-us");
        errorBuilder.AddMimeType("document-format-supported", kDefaultDocumentFormat);
        errorBuilder.AddKeyword("status-message", "unsupported document-format: " + format);
        errorBuilder.EndAttributes();
        builder = errorBuilder;
        return;
    }

    if (!request.hasDocument || totalBodyBytes <= request.documentOffset)
    {
        Ipp::ResponseBuilder errorBuilder;
        errorBuilder.Begin((unsigned char)request.majorVersion, (unsigned char)request.minorVersion,
                           Ipp::Status::ClientErrorBadRequest, request.requestId);
        errorBuilder.BeginGroup(Ipp::Tag::OperationAttributes);
        errorBuilder.AddCharset("attributes-charset", "utf-8");
        errorBuilder.AddNaturalLanguage("attributes-natural-language", "en-us");
        errorBuilder.AddKeyword("status-message", "print-job 未携带文档数据");
        errorBuilder.EndAttributes();
        builder = errorBuilder;
        return;
    }

    std::string jobName = FirstText(request, "job-name", "Untitled");
    std::string userName = FirstText(request, "requesting-user-name", "anonymous");

    IppJobPtr job = CreateJob(printerIndex, jobName, userName, format);
    job->copies = ReadCopies(request);
    job->tempPathUtf8 = WideToUtf8(bodyFilePath);
    job->documentOffset = request.documentOffset;
    job->documentBytes = (long long)(totalBodyBytes - request.documentOffset);
    job->documentAttached = true;

    job->stateReasons = "job-queued";
    EnqueueJob(job);

    // 文档文件所有权已转移给作业，请求线程不得删除
    bodyClaimed = true;

    builder.AddUri("job-uri", JobUriFor(job));
    builder.AddInt("job-id", job->id);
    builder.AddEnum("job-state", job->state);
    builder.AddKeyword("job-state-reasons", job->stateReasons);
    builder.AddInt("job-k-octets", (long long)(job->documentBytes / 1024));
    builder.AddMimeType("document-format", format);
}

void IppServer::HandleCreateJob(const Ipp::Request& request, int printerIndex,
                                Ipp::ResponseBuilder& builder)
{
    builder.BeginGroup(Ipp::Tag::OperationAttributes);
    AddOperationAttributes(builder);

    std::string jobName = FirstText(request, "job-name", "Untitled");
    std::string userName = FirstText(request, "requesting-user-name", "anonymous");
    std::string format = FirstText(request, "document-format", kDefaultDocumentFormat);

    IppJobPtr job = CreateJob(printerIndex, jobName, userName, format);
    job->copies = ReadCopies(request);

    builder.AddUri("job-uri", JobUriFor(job));
    builder.AddInt("job-id", job->id);
    builder.AddEnum("job-state", job->state);
    builder.AddKeyword("job-state-reasons", job->stateReasons);
}

void IppServer::HandleSendDocument(const Ipp::Request& request, int printerIndex,
                                   const std::wstring& bodyFilePath, size_t totalBodyBytes,
                                   bool& bodyClaimed, Ipp::ResponseBuilder& builder)
{
    builder.BeginGroup(Ipp::Tag::OperationAttributes);
    AddOperationAttributes(builder);

    int jobId = ResolveJobId(FirstText(request, "job-uri", ""));
    if (jobId <= 0)
    {
        long long value = 0;
        if (request.GetInt(Ipp::Tag::OperationAttributes, "job-id", value))
            jobId = (int)value;
    }

    bool lastDocument = true;
    request.GetBool(Ipp::Tag::OperationAttributes, "last-document", lastDocument);

    std::string format = FirstText(request, "document-format", kDefaultDocumentFormat);
    if (!IsFormatSupported(printerIndex, format))
    {
        Ipp::ResponseBuilder errorBuilder;
        errorBuilder.Begin((unsigned char)request.majorVersion, (unsigned char)request.minorVersion,
                           Ipp::Status::ClientErrorDocumentFormatNotSupported, request.requestId);
        errorBuilder.BeginGroup(Ipp::Tag::OperationAttributes);
        errorBuilder.AddCharset("attributes-charset", "utf-8");
        errorBuilder.AddNaturalLanguage("attributes-natural-language", "en-us");
        errorBuilder.AddKeyword("status-message", "unsupported document-format: " + format);
        errorBuilder.EndAttributes();
        builder = errorBuilder;
        return;
    }

    IppJobPtr job = FindJob(jobId);
    const bool usable = job && job->printerIndex == printerIndex && job->state < IppJobCanceled;

    if (!usable)
    {
        // ===== 容错（Windows IPP 端口重试场景）=====
        // 客户端重试时常常仍带上一单的 job-id（服务端已递增/已完成），
        // 严格按 RFC 回 not-found / not-possible 会让整单打印永远发不出去。
        // 这里按 Print-Job 语义兜底：
        //   1) 优先复用本打印机上"刚创建但还没收到文档"的作业（正常 Create-Job 流程）；
        //   2) 没有则新建一个作业。
        job = FindPendingJobWithoutDocument(printerIndex);

        if (!job)
        {
            job = CreateJob(printerIndex, FirstText(request, "job-name", "Untitled"),
                            FirstText(request, "requesting-user-name", "anonymous"), format);
            Log("Send-Document: job-id " + std::to_string(jobId) +
                " 无效或已结束，已新建作业 #" + std::to_string(job->id));
        }
        else
        {
            Log("Send-Document: job-id " + std::to_string(jobId) +
                " 无效或已结束，已绑定到待收文档的作业 #" + std::to_string(job->id));
        }
    }

    job->documentFormat = format;
    // copies 通常在 Create-Job 时给出，这里只在显式携带时才覆盖
    if (request.Find(Ipp::Tag::OperationAttributes, "copies") != nullptr)
        job->copies = ReadCopies(request);
    job->tempPathUtf8 = WideToUtf8(bodyFilePath);
    job->documentOffset = request.documentOffset;
    job->documentBytes = (long long)(totalBodyBytes > request.documentOffset
                                         ? totalBodyBytes - request.documentOffset
                                         : 0);
    job->documentAttached = true;
    bodyClaimed = true;

    if (lastDocument)
    {
        job->stateReasons = "job-queued";
        EnqueueJob(job);
    }

    builder.AddUri("job-uri", JobUriFor(job));
    builder.AddInt("job-id", job->id);
    builder.AddEnum("job-state", job->state);
    builder.AddKeyword("job-state-reasons", job->stateReasons);
}

void IppServer::HandleGetJobs(const Ipp::Request& request, int printerIndex,
                              Ipp::ResponseBuilder& builder)
{
    builder.BeginGroup(Ipp::Tag::OperationAttributes);
    AddOperationAttributes(builder);

    std::string whichJobs = FirstText(request, "which-jobs", "not-completed");
    std::string lowerWhich = ToLowerCopy(whichJobs);

    long long limit = 0;
    request.GetInt(Ipp::Tag::OperationAttributes, "limit", limit);

    bool myJobs = false;
    request.GetBool(Ipp::Tag::OperationAttributes, "my-jobs", myJobs);
    std::string userName = FirstText(request, "requesting-user-name", "");

    std::vector<IppJobPtr> snapshot;
    {
        std::lock_guard<std::mutex> guard(m_jobsMutex);
        for (const auto& entry : m_jobs)
        {
            const IppJobPtr& job = entry.second;
            if (job->printerIndex != printerIndex)
                continue;

            bool completed = (job->state >= IppJobCanceled);
            if (lowerWhich == "completed" && !completed)
                continue;
            if ((lowerWhich == "not-completed" || lowerWhich.empty()) && completed)
                continue;

            if (myJobs && !userName.empty() &&
                _stricmp(job->userName.c_str(), userName.c_str()) != 0)
            {
                continue;
            }

            snapshot.push_back(job);
        }
    }

    if (limit > 0 && (long long)snapshot.size() > limit)
        snapshot.resize((size_t)limit);

    for (const IppJobPtr& job : snapshot)
        AddJobAttributes(builder, job);
}

void IppServer::HandleGetJobAttributes(const Ipp::Request& request, int /*printerIndex*/,
                                       Ipp::ResponseBuilder& builder)
{
    builder.BeginGroup(Ipp::Tag::OperationAttributes);
    AddOperationAttributes(builder);

    int jobId = ResolveJobId(FirstText(request, "job-uri", ""));
    if (jobId <= 0)
    {
        long long value = 0;
        if (request.GetInt(Ipp::Tag::OperationAttributes, "job-id", value))
            jobId = (int)value;
    }

    IppJobPtr job = FindJob(jobId);
    if (!job)
    {
        Ipp::ResponseBuilder errorBuilder;
        errorBuilder.Begin((unsigned char)request.majorVersion, (unsigned char)request.minorVersion,
                           Ipp::Status::ClientErrorNotFound, request.requestId);
        errorBuilder.BeginGroup(Ipp::Tag::OperationAttributes);
        errorBuilder.AddCharset("attributes-charset", "utf-8");
        errorBuilder.AddNaturalLanguage("attributes-natural-language", "en-us");
        errorBuilder.AddKeyword("status-message", "job not found: " + std::to_string(jobId));
        errorBuilder.EndAttributes();
        builder = errorBuilder;
        return;
    }

    AddJobAttributes(builder, job);
}

void IppServer::HandleCancelJob(const Ipp::Request& request, int /*printerIndex*/,
                                Ipp::ResponseBuilder& builder)
{
    builder.BeginGroup(Ipp::Tag::OperationAttributes);
    AddOperationAttributes(builder);

    int jobId = ResolveJobId(FirstText(request, "job-uri", ""));
    if (jobId <= 0)
    {
        long long value = 0;
        if (request.GetInt(Ipp::Tag::OperationAttributes, "job-id", value))
            jobId = (int)value;
    }

    IppJobPtr job = FindJob(jobId);
    if (!job)
    {
        Ipp::ResponseBuilder errorBuilder;
        errorBuilder.Begin((unsigned char)request.majorVersion, (unsigned char)request.minorVersion,
                           Ipp::Status::ClientErrorNotFound, request.requestId);
        errorBuilder.BeginGroup(Ipp::Tag::OperationAttributes);
        errorBuilder.AddCharset("attributes-charset", "utf-8");
        errorBuilder.AddNaturalLanguage("attributes-natural-language", "en-us");
        errorBuilder.AddKeyword("status-message", "job not found: " + std::to_string(jobId));
        errorBuilder.EndAttributes();
        builder = errorBuilder;
        return;
    }

    // 置取消标记：
    //   - 排队中的作业：worker 取出时立刻跳过；
    //   - 正在打印的作业：worker 在写下一块数据前中止，并用 AbortPrinter 丢弃半页。
    // 这里不重复入队（已入队的作业本就在队列里）。
    job->cancelRequested.store(true);

    {
        std::lock_guard<std::mutex> guard(m_jobsMutex);
        if (job->state == IppJobPending)
        {
            job->state = IppJobCanceled;
            job->stateReasons = "job-canceled-by-user";
            job->completedUpTime = UpTime();
        }
    }
}

// Close-Job：Create-Job 后客户端可能用多次 Send-Document(last-document=false)
// 逐个提交文档，最后用 Close-Job 收尾。这里把尚未入队的作业正式排队。
void IppServer::HandleCloseJob(const Ipp::Request& request, int printerIndex,
                               Ipp::ResponseBuilder& builder)
{
    builder.BeginGroup(Ipp::Tag::OperationAttributes);
    AddOperationAttributes(builder);

    int jobId = ResolveJobId(FirstText(request, "job-uri", ""));
    if (jobId <= 0)
    {
        long long value = 0;
        if (request.GetInt(Ipp::Tag::OperationAttributes, "job-id", value))
            jobId = (int)value;
    }

    IppJobPtr job = FindJob(jobId);
    if (!job || job->printerIndex != printerIndex)
    {
        Ipp::ResponseBuilder errorBuilder;
        errorBuilder.Begin((unsigned char)request.majorVersion, (unsigned char)request.minorVersion,
                           Ipp::Status::ClientErrorNotFound, request.requestId);
        errorBuilder.BeginGroup(Ipp::Tag::OperationAttributes);
        errorBuilder.AddCharset("attributes-charset", "utf-8");
        errorBuilder.AddNaturalLanguage("attributes-natural-language", "en-us");
        errorBuilder.AddKeyword("status-message", "job not found: " + std::to_string(jobId));
        errorBuilder.EndAttributes();
        builder = errorBuilder;
        return;
    }

    if (job->state < IppJobCanceled)
    {
        if (!job->documentAttached)
        {
            Ipp::ResponseBuilder errorBuilder;
            errorBuilder.Begin((unsigned char)request.majorVersion,
                               (unsigned char)request.minorVersion,
                               Ipp::Status::ClientErrorNotPossible, request.requestId);
            errorBuilder.BeginGroup(Ipp::Tag::OperationAttributes);
            errorBuilder.AddCharset("attributes-charset", "utf-8");
            errorBuilder.AddNaturalLanguage("attributes-natural-language", "en-us");
            errorBuilder.AddKeyword("status-message", "job has no document");
            errorBuilder.EndAttributes();
            builder = errorBuilder;
            return;
        }

        if (job->state == IppJobPending)
        {
            job->stateReasons = "job-queued";
            EnqueueJob(job);
        }
    }

    builder.AddUri("job-uri", JobUriFor(job));
    builder.AddInt("job-id", job->id);
    builder.AddEnum("job-state", job->state);
    builder.AddKeyword("job-state-reasons", job->stateReasons);
}

void IppServer::HandleHoldJob(const Ipp::Request& request, int printerIndex, bool hold,
                              Ipp::ResponseBuilder& builder)
{
    builder.BeginGroup(Ipp::Tag::OperationAttributes);
    AddOperationAttributes(builder);

    int jobId = ResolveJobId(FirstText(request, "job-uri", ""));
    if (jobId <= 0)
    {
        long long value = 0;
        if (request.GetInt(Ipp::Tag::OperationAttributes, "job-id", value))
            jobId = (int)value;
    }

    IppJobPtr job = FindJob(jobId);
    if (!job || job->printerIndex != printerIndex)
    {
        Ipp::ResponseBuilder errorBuilder;
        errorBuilder.Begin((unsigned char)request.majorVersion, (unsigned char)request.minorVersion,
                           Ipp::Status::ClientErrorNotFound, request.requestId);
        errorBuilder.BeginGroup(Ipp::Tag::OperationAttributes);
        errorBuilder.AddCharset("attributes-charset", "utf-8");
        errorBuilder.AddNaturalLanguage("attributes-natural-language", "en-us");
        errorBuilder.AddKeyword("status-message", "job not found: " + std::to_string(jobId));
        errorBuilder.EndAttributes();
        builder = errorBuilder;
        return;
    }

    bool enqueue = false;
    {
        std::lock_guard<std::mutex> guard(m_jobsMutex);

        if (hold)
        {
            if (job->state == IppJobPending)
            {
                job->state = IppJobPendingHeld;
                job->stateReasons = "job-hold-until-specified";
            }
            else if (job->state != IppJobPendingHeld)
            {
                Ipp::ResponseBuilder errorBuilder;
                errorBuilder.Begin((unsigned char)request.majorVersion,
                                   (unsigned char)request.minorVersion,
                                   Ipp::Status::ClientErrorNotPossible, request.requestId);
                errorBuilder.BeginGroup(Ipp::Tag::OperationAttributes);
                errorBuilder.AddCharset("attributes-charset", "utf-8");
                errorBuilder.AddNaturalLanguage("attributes-natural-language", "en-us");
                errorBuilder.AddKeyword("status-message", "job already processing or finished");
                errorBuilder.EndAttributes();
                builder = errorBuilder;
                return;
            }
        }
        else
        {
            if (job->state == IppJobPendingHeld)
            {
                job->state = IppJobPending;
                job->stateReasons = "job-queued";
                enqueue = job->documentAttached;
            }
            else if (job->state == IppJobPending && job->documentAttached)
            {
                enqueue = true;   // 已在排队，唤醒 worker 即可
            }
            else if (job->state >= IppJobCanceled)
            {
                Ipp::ResponseBuilder errorBuilder;
                errorBuilder.Begin((unsigned char)request.majorVersion,
                                   (unsigned char)request.minorVersion,
                                   Ipp::Status::ClientErrorNotPossible, request.requestId);
                errorBuilder.BeginGroup(Ipp::Tag::OperationAttributes);
                errorBuilder.AddCharset("attributes-charset", "utf-8");
                errorBuilder.AddNaturalLanguage("attributes-natural-language", "en-us");
                errorBuilder.AddKeyword("status-message", "job already finished");
                errorBuilder.EndAttributes();
                builder = errorBuilder;
                return;
            }
        }
    }

    if (enqueue)
        EnqueueJob(job);

    builder.AddUri("job-uri", JobUriFor(job));
    builder.AddInt("job-id", job->id);
    builder.AddEnum("job-state", job->state);
    builder.AddKeyword("job-state-reasons", job->stateReasons);
}

void IppServer::HandleRestartJob(const Ipp::Request& request, int printerIndex,
                                 Ipp::ResponseBuilder& builder)
{
    builder.BeginGroup(Ipp::Tag::OperationAttributes);
    AddOperationAttributes(builder);

    int jobId = ResolveJobId(FirstText(request, "job-uri", ""));
    if (jobId <= 0)
    {
        long long value = 0;
        if (request.GetInt(Ipp::Tag::OperationAttributes, "job-id", value))
            jobId = (int)value;
    }

    IppJobPtr job = FindJob(jobId);
    if (!job || job->printerIndex != printerIndex)
    {
        Ipp::ResponseBuilder errorBuilder;
        errorBuilder.Begin((unsigned char)request.majorVersion, (unsigned char)request.minorVersion,
                           Ipp::Status::ClientErrorNotFound, request.requestId);
        errorBuilder.BeginGroup(Ipp::Tag::OperationAttributes);
        errorBuilder.AddCharset("attributes-charset", "utf-8");
        errorBuilder.AddNaturalLanguage("attributes-natural-language", "en-us");
        errorBuilder.AddKeyword("status-message", "job not found: " + std::to_string(jobId));
        errorBuilder.EndAttributes();
        builder = errorBuilder;
        return;
    }

    // 作业完成后文档临时文件即被释放（不再占用磁盘），因此无法重打。
    // 按 RFC 8011 返回 client-error-not-possible，而不是假装成功。
    Ipp::ResponseBuilder errorBuilder;
    errorBuilder.Begin((unsigned char)request.majorVersion, (unsigned char)request.minorVersion,
                       Ipp::Status::ClientErrorNotPossible, request.requestId);
    errorBuilder.BeginGroup(Ipp::Tag::OperationAttributes);
    errorBuilder.AddCharset("attributes-charset", "utf-8");
    errorBuilder.AddNaturalLanguage("attributes-natural-language", "en-us");
    errorBuilder.AddKeyword("status-message", "document is no longer retained");
    errorBuilder.EndAttributes();
    builder = errorBuilder;
}

void IppServer::HandlePurgeJobs(const Ipp::Request& request, int printerIndex,
                                Ipp::ResponseBuilder& builder)
{
    builder.BeginGroup(Ipp::Tag::OperationAttributes);
    AddOperationAttributes(builder);

    std::vector<IppJobPtr> targets;
    {
        std::lock_guard<std::mutex> guard(m_jobsMutex);
        for (const auto& entry : m_jobs)
        {
            const IppJobPtr& job = entry.second;
            if (job->printerIndex != printerIndex)
                continue;
            if (job->state >= IppJobCanceled)
                continue;

            job->cancelRequested.store(true);
            if (job->state == IppJobPending)
            {
                job->state = IppJobCanceled;
                job->stateReasons = "job-canceled-by-user";
                job->completedUpTime = UpTime();
            }
            targets.push_back(job);
        }
    }

    for (const IppJobPtr& job : targets)
        RemoveJobFiles(job);

    (void)request;
}

void IppServer::HandleIdentifyPrinter(const Ipp::Request& request, int printerIndex,
                                      Ipp::ResponseBuilder& builder)
{
    builder.BeginGroup(Ipp::Tag::OperationAttributes);
    AddOperationAttributes(builder);

    std::vector<std::string> actions = request.GetTextList(Ipp::Tag::OperationAttributes,
                                                           "identify-actions");
    std::string joined;
    for (size_t i = 0; i < actions.size(); ++i)
    {
        if (i > 0)
            joined += ",";
        joined += actions[i];
    }
    if (joined.empty())
        joined = "display";

    // Phase A：登记日志即视为已执行物理动作（后续可接入托盘提示/闪灯）
    Log("Identify-Printer: 打印机#" + std::to_string(printerIndex) + " 动作=" + joined);
}

void IppServer::HandlePauseResume(const Ipp::Request& request, int printerIndex, bool pause,
                                  Ipp::ResponseBuilder& builder)
{
    builder.BeginGroup(Ipp::Tag::OperationAttributes);
    AddOperationAttributes(builder);

    if (printerIndex < 0 || printerIndex >= 64)
    {
        Log("Pause/Resume-Printer: 打印机编号超出范围 " + std::to_string(printerIndex));
        return;
    }

    unsigned long long mask = m_pausedMask.load();
    if (pause)
        mask |= (1ULL << printerIndex);
    else
        mask &= ~(1ULL << printerIndex);
    m_pausedMask.store(mask);

    Log(std::string(pause ? "已暂停" : "已恢复") + " 打印机#" + std::to_string(printerIndex));

    (void)request;
}

// ===========================================================================
// 作业管理
// ===========================================================================

IppJobPtr IppServer::CreateJob(int printerIndex, const std::string& name,
                               const std::string& userName, const std::string& documentFormat)
{
    IppJobPtr job(new IppJob());
    job->printerIndex = printerIndex;
    job->name = name.empty() ? "Untitled" : name;
    job->userName = userName.empty() ? "anonymous" : userName;
    job->documentFormat = documentFormat.empty() ? kDefaultDocumentFormat : documentFormat;
    job->state = IppJobPending;
    // 刚创建、尚未收到任何文档（对应 RFC 8011 的 job-data-insufficient）
    job->stateReasons = "job-data-insufficient";
    job->createdUpTime = UpTime();

    std::lock_guard<std::mutex> guard(m_jobsMutex);

    job->id = m_nextJobId++;
    m_jobs[job->id] = job;

    // 控制作业表规模：超出上限时先清理已结束的最老作业
    if (m_jobs.size() > kJobRetentionMax)
    {
        for (auto it = m_jobs.begin(); it != m_jobs.end() && m_jobs.size() > kJobRetentionMax;)
        {
            if (it->second->state >= IppJobCanceled)
                it = m_jobs.erase(it);
            else
                ++it;
        }
    }

    return job;
}

IppJobPtr IppServer::FindJob(int jobId)
{
    if (jobId <= 0)
        return nullptr;

    std::lock_guard<std::mutex> guard(m_jobsMutex);
    auto it = m_jobs.find(jobId);
    if (it == m_jobs.end())
        return nullptr;
    return it->second;
}

void IppServer::EnqueueJob(const IppJobPtr& job)
{
    if (!job || job->printerIndex < 0 || job->printerIndex >= (int)m_runtimes.size())
        return;

    PrinterRuntime& runtime = *m_runtimes[job->printerIndex];

    // 已在队列中的作业不重复入队（Cancel-Job 之后会再调一次用于唤醒）
    bool alreadyQueued = false;
    {
        std::lock_guard<std::mutex> guard(runtime.mutex);
        for (const IppJobPtr& item : runtime.queue)
        {
            if (item->id == job->id)
            {
                alreadyQueued = true;
                break;
            }
        }

        // 只负责入队，不修改作业状态（状态由各操作显式设置，避免"复活"已取消的作业）
        if (!alreadyQueued)
            runtime.queue.push_back(job);
    }

    runtime.condition.notify_all();
}

void IppServer::RemoveJobFiles(const IppJobPtr& job)
{
    if (!job || job->tempPathUtf8.empty())
        return;

    ::DeleteFileW(Utf8ToWide(job->tempPathUtf8).c_str());
    job->tempPathUtf8.clear();
}

int IppServer::QueuedCount(int printerIndex) const
{
    int count = 0;

    std::lock_guard<std::mutex> guard(m_jobsMutex);
    for (const auto& entry : m_jobs)
    {
        const IppJobPtr& job = entry.second;
        if (job->printerIndex == printerIndex && job->state < IppJobCanceled)
            ++count;
    }

    return count;
}

int IppServer::PrinterState(int printerIndex) const
{
    const PrinterRuntime& runtime = *m_runtimes[printerIndex];
    if (runtime.currentJobId.load() != 0)
        return 4;   // processing
    return 3;       // idle
}

bool IppServer::IsFormatSupported(int printerIndex, const std::string& format) const
{
    if (format.empty())
        return true;

    if (printerIndex < 0 || printerIndex >= (int)m_runtimes.size())
        return false;

    // Phase B：系统具备 PDF 栅格化能力时接受 application/pdf
    if (m_pdfRenderingEnabled && IsPdfFormatName(format))
        return true;

    for (const std::string& item : m_runtimes[printerIndex]->pdlFormats)
    {
        if (_stricmp(item.c_str(), format.c_str()) == 0)
            return true;
    }

    return false;
}

std::string IppServer::PrinterUriFor(int printerIndex, const std::string& address) const
{
    std::string base = "ipp://" + address + ":" + std::to_string(m_port) + "/ipp/print";

    if (printerIndex < 0 || printerIndex >= (int)m_printers.size())
        return base;

    // 末尾编号直接用 RAW 端口号（PrinterConfig::index = 端口）：
    // 9100 -> /ipp/print/9100，9101 -> /ipp/print/9101，与 RAW 端口一一对应。
    // 同时 /ipp/print 始终指向第一台，兼容只用标准路径的客户端。
    const int token = m_printers[printerIndex].index;
    if (token <= 0)
        return base;

    return base + "/" + std::to_string(token);
}

std::string IppServer::JobUriFor(const IppJobPtr& job) const
{
    if (!job)
        return std::string();

    // 标准形态：ipp://host:port/jobs/<job-id>（CUPS 与 Windows IPP 客户端都用这种）
    return "ipp://" + m_primaryAddress + ":" + std::to_string(m_port) + "/jobs/" +
           std::to_string(job->id);
}

IppJobPtr IppServer::FindPendingJobWithoutDocument(int printerIndex)
{
    IppJobPtr found;

    std::lock_guard<std::mutex> guard(m_jobsMutex);
    for (const auto& entry : m_jobs)
    {
        const IppJobPtr& job = entry.second;
        if (job->printerIndex != printerIndex)
            continue;
        if (job->state != IppJobPending || job->documentAttached)
            continue;
        if (!found || job->id > found->id)
            found = job;
    }

    return found;
}

int IppServer::ResolvePrinterIndex(const std::string& printerUri) const
{
    if (printerUri.empty())
        return 0;

    size_t slash = printerUri.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= printerUri.size())
        return 0;

    std::string tail = printerUri.substr(slash + 1);

    bool digits = !tail.empty();
    for (char c : tail)
    {
        if (c < '0' || c > '9')
        {
            digits = false;
            break;
        }
    }

    if (!digits)
        return 0;

    const int token = atoi(tail.c_str());

    // /ipp/print 指向第一台打印机
    if (token <= 0)
        return 0;

    // 形式一：/ipp/print/9100 —— 直接是 RAW 端口号（与配置一致，推荐）
    for (size_t i = 0; i < m_printers.size(); ++i)
    {
        if (m_printers[i].index == token)
            return (int)i;
    }

    // 形式二：/ipp/print/1 —— 端口号相对 9100 的偏移（兼容旧客户端与手输 URL）
    for (size_t i = 0; i < m_printers.size(); ++i)
    {
        if (m_printers[i].port == token + 9100)
            return (int)i;
    }

    // 找不到：返回 -1，让上层回 client-error-not-found，
    // 而不是错位打到另一台打印机上。
    return -1;
}

int IppServer::ResolveJobId(const std::string& jobUri) const
{
    if (jobUri.empty())
        return 0;

    size_t slash = jobUri.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= jobUri.size())
        return 0;

    return atoi(jobUri.substr(slash + 1).c_str());
}

std::string IppServer::SanitizeName(const std::string& name) const
{
    std::string out;
    out.reserve(name.size());

    for (char c : name)
    {
        if (c == ' ' || c == '\t' || c == '"' || c == '#' || c == '/')
            out.push_back('_');
        else
            out.push_back(c);
    }

    if (out.empty())
        out = "printer";

    return out;
}

std::wstring IppServer::MakeJobTempPath() const
{
    wchar_t tempPath[MAX_PATH] = { 0 };
    ::GetTempPathW(MAX_PATH, tempPath);

    std::wstring dir = std::wstring(tempPath) + L"PrintServerIpp";
    ::CreateDirectoryW(dir.c_str(), nullptr);

    static std::atomic<unsigned long> counter(0);
    unsigned long seq = counter.fetch_add(1);

    wchar_t name[128] = { 0 };
    _snwprintf_s(name, _countof(name), _TRUNCATE, L"\\job-%lu-%lu-%llu.bin",
                 (unsigned long)::GetCurrentProcessId(), seq,
                 (unsigned long long)::GetTickCount64());

    return dir + name;
}

long long IppServer::UpTime() const
{
    return (long long)::time(nullptr) - m_startTime;
}

// ===========================================================================
// 文档提交（Phase B：按格式分流）
// ===========================================================================

std::wstring IppServer::MaterializeDocument(const IppJobPtr& job, const wchar_t* extension,
                                            std::string& error) const
{
    error.clear();

    if (job->tempPathUtf8.empty() || job->documentBytes <= 0)
    {
        error = "作业文档为空";
        return std::wstring();
    }

    // PDF 渲染要求文件以 %PDF 开头，而作业临时文件前面还带着 IPP 属性区，
    // 因此把文档段单独落地成一个新文件。
    std::wstring target = MakeJobTempPath();
    size_t dot = target.find_last_of(L'.');
    if (dot != std::wstring::npos)
        target = target.substr(0, dot);
    target += (extension != nullptr) ? extension : L"";

    HANDLE source = ::CreateFileW(Utf8ToWide(job->tempPathUtf8).c_str(), GENERIC_READ,
                                 FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source == INVALID_HANDLE_VALUE)
    {
        error = "无法打开作业临时文件";
        return std::wstring();
    }

    LARGE_INTEGER offset;
    offset.QuadPart = (LONGLONG)job->documentOffset;
    if (!::SetFilePointerEx(source, offset, nullptr, FILE_BEGIN))
    {
        ::CloseHandle(source);
        error = "定位文档偏移失败";
        return std::wstring();
    }

    HANDLE destination = ::CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (destination == INVALID_HANDLE_VALUE)
    {
        ::CloseHandle(source);
        error = "无法创建文档临时文件";
        return std::wstring();
    }

    std::vector<unsigned char> buffer(64 * 1024);
    long long remaining = job->documentBytes;
    bool ok = true;

    while (remaining > 0)
    {
        DWORD want = (remaining < (long long)buffer.size()) ? (DWORD)remaining
                                                            : (DWORD)buffer.size();
        DWORD read = 0;
        if (!::ReadFile(source, buffer.data(), want, &read, nullptr) || read == 0)
        {
            ok = false;
            break;
        }

        DWORD written = 0;
        if (!::WriteFile(destination, buffer.data(), read, &written, nullptr) || written != read)
        {
            ok = false;
            break;
        }

        remaining -= read;
    }

    ::CloseHandle(destination);
    ::CloseHandle(source);

    if (!ok)
    {
        ::DeleteFileW(target.c_str());
        error = "复制文档数据失败";
        return std::wstring();
    }

    return target;
}

bool IppServer::PrintJobDocument(const IppJobPtr& job, int printerIndex, bool& canceled,
                                 std::string& error)
{
    error.clear();
    canceled = false;

    if (printerIndex < 0 || printerIndex >= (int)m_printers.size())
    {
        error = "打印机序号无效";
        return false;
    }

    const std::string printerName = m_printers[printerIndex].name;
    const int copies = (job->copies > 0) ? job->copies : 1;

    // ---------- Phase B：PDF 走栅格化 + XPS，交给本机驱动渲染 ----------
    if (m_pdfRenderingEnabled && IsPdfFormatName(job->documentFormat))
    {
        std::wstring pdfPath = MaterializeDocument(job, L".pdf", error);
        if (pdfPath.empty())
            return false;

        const std::string pdfPathUtf8 = WideToUtf8(pdfPath);

        // 按打印机自身分辨率渲染：只按 300 dpi 渲染再让 600 dpi 打印机放大，
        // 线条和小字会明显发虚（"不如 USB 直打清楚"就是这个原因）。
        // 上限 600 dpi：A4@600 已是约 4900x7000 像素（单页位图约 140 MB），
        // 再高收益有限、内存风险大。
        int renderDpi = m_renderDpi;
        {
            int printerDpiX = 0;
            int printerDpiY = 0;
            if (IppRaster::GetPrinterResolution(printerName, printerDpiX, printerDpiY))
            {
                const int printerDpi = (printerDpiX > printerDpiY) ? printerDpiX : printerDpiY;
                if (printerDpi > renderDpi)
                    renderDpi = printerDpi;
            }

            // 至少按 600 dpi 渲染：即便驱动只接受 300 dpi，600 dpi 位图
            // 降采样也比直接 300 dpi 平滑（等效超采样）；上限同为 600。
            if (renderDpi < 600)
                renderDpi = 600;
            if (renderDpi > 600)
                renderDpi = 600;
        }

        Log("作业 #" + std::to_string(job->id) + " PDF 渲染分辨率: " +
            std::to_string(renderDpi) + " dpi");

        // 目标打印机是否为彩色以驱动探测为准（配置项只在探测失败时兜底）
        const bool printerColor =
            (m_runtimes[printerIndex] != nullptr && m_runtimes[printerIndex]->caps.valid)
                ? m_runtimes[printerIndex]->caps.color
                : m_colorSupported;

        // 黑白机：位图按亮度二值化；彩色机：保持 PDFium 原始渲染
        const bool monochromeOutput = !printerColor;

        long long totalPages = 0;
        bool ok = true;

        for (int copy = 0; copy < copies && ok; ++copy)
        {
            if (job->cancelRequested.load())
            {
                canceled = true;
                break;
            }

            IppPdf::Document document;
            std::string openError;
            if (!document.Open(pdfPathUtf8, openError))
            {
                error = openError;
                ok = false;
                break;
            }

            const int pageCount = document.PageCount();
            if (pageCount <= 0)
            {
                error = "PDF 中没有可打印页面";
                ok = false;
                break;
            }

            // 纸张跟随 PDF 首页尺寸（点 → 毫米）。不这么做的话，输出完全取决于
            // 服务端驱动当前的默认纸张：Letter 会被缩到 A4、A5 会被放大、
            // 横向页面会被塞进纵向纸张。
            double paperWidthMm = 0.0;
            double paperHeightMm = 0.0;
            {
                double widthPt = 0.0;
                double heightPt = 0.0;
                if (document.GetPageSize(0, widthPt, heightPt))
                {
                    paperWidthMm = widthPt / 72.0 * 25.4;
                    paperHeightMm = heightPt / 72.0 * 25.4;
                }
            }

            // 大幅面纸张的内存保护：A3/A2/绘图仪按 600 dpi 渲染会直接吃掉几百 MB，
            // 这里按面积限制单页位图在 6000 万像素以内（A4@600 约 3400 万）。
            if (paperWidthMm > 1.0 && paperHeightMm > 1.0)
            {
                const double areaInches = (paperWidthMm / 25.4) * (paperHeightMm / 25.4);
                const double maxDpi = sqrt(60000000.0 / areaInches);

                if ((double)renderDpi > maxDpi)
                {
                    renderDpi = (int)maxDpi;
                    if (renderDpi < 150)
                        renderDpi = 150;

                    Log("作业 #" + std::to_string(job->id) + " 纸张 " +
                        std::to_string((int)paperWidthMm) + "x" +
                        std::to_string((int)paperHeightMm) + "mm 较大，渲染分辨率降为 " +
                        std::to_string(renderDpi) + " dpi");
                }
            }

            // 按需渲染，并直接用打印机 DC 输出（GDI 位图路径）。
            // 不再组装 XPS 包：XpsPrint 对部件资源 URI 与驱动的兼容性不稳定，
            // 现场实测会打出纯白页；GDI 对所有 v3/v4 驱动都有效。
            IppRaster::RasterPage currentPage;
            std::string submitError;

            const bool submitted = IppRaster::PrintPagesViaGdi(
                printerName, (size_t)pageCount,
                [&document, &currentPage, renderDpi, monochromeOutput](size_t index,
                                                                       std::string& pageError)
                    -> const IppRaster::RasterPage* {
                    if (!document.RenderPage((int)index, renderDpi, currentPage, pageError))
                        return nullptr;

                    // 诊断：把首页渲染结果原样存成 PNG（增强之前），
                    // 用于判断"位图本身是否清晰"——是渲染器的问题还是打印管线的问题。
                    if (index == 0)
                    {
                        std::vector<unsigned char> png;
                        std::string pngError;
                        if (IppRaster::EncodePngRgb(currentPage, png, pngError))
                        {
                            const wchar_t* kDir = L"C:\\ProgramData\\PrintServer";
                            ::CreateDirectoryW(kDir, nullptr);

                            std::wstring path = std::wstring(kDir) + L"\\last_page.png";
                            HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                            if (file != INVALID_HANDLE_VALUE)
                            {
                                DWORD written = 0;
                                ::WriteFile(file, png.data(), (DWORD)png.size(), &written, nullptr);
                                ::CloseHandle(file);
                            }
                        }
                    }

                    // 打印前增强：去掉浅灰底、把文字主体压成纯黑，
                    // 减少激光打印机 halftone 网点造成的"发虚/发淡"观感。
                    IppRaster::EnhanceForPrint(currentPage, monochromeOutput);

                    return &currentPage;
                },
                job->name, submitError, paperWidthMm, paperHeightMm);

            char paperText[48] = { 0 };
            if (paperWidthMm > 1.0 && paperHeightMm > 1.0)
            {
                _snprintf_s(paperText, sizeof(paperText), _TRUNCATE, "%.0fx%.0fmm",
                            paperWidthMm, paperHeightMm);
            }

            Log("作业 #" + std::to_string(job->id) + " GDI 打印到「" + printerName + "」: " +
                std::to_string(pageCount) + " 页" +
                (paperText[0] != '\0' ? (std::string(", 纸张 ") + paperText) : std::string()) +
                " -> " + (submitted ? std::string("已提交") : ("失败: " + submitError)));

            if (!submitted)
            {
                error = submitError;
                ok = false;
                break;
            }

            totalPages += pageCount;
        }

        ::DeleteFileW(pdfPath.c_str());

        if (ok)
        {
            Log("作业 #" + std::to_string(job->id) + " PDF 免驱打印完成: " +
                std::to_string(totalPages) + " 页 / " + std::to_string(copies) + " 份");
        }

        return ok;
    }

    // ---------- 其余：RAW 直通（设备原生语言）----------
    for (int copy = 0; copy < copies; ++copy)
    {
        bool copyCanceled = false;

        if (!RawPrinter::SendFileRangeToPrinter(
                printerName, Utf8ToWide(job->tempPathUtf8), (long long)job->documentOffset,
                job->documentBytes, [&job]() { return job->cancelRequested.load(); },
                copyCanceled))
        {
            canceled = copyCanceled;
            if (!copyCanceled)
                error = "RAW 直通打印失败";
            return false;
        }

        if (job->cancelRequested.load())
        {
            canceled = true;
            return false;
        }
    }

    return true;
}

// ===========================================================================
// 打印 worker
// ===========================================================================

void IppServer::WorkerLoop(int printerIndex)
{
    PrinterRuntime& runtime = *m_runtimes[printerIndex];

    for (;;)
    {
        IppJobPtr job;

        {
            std::unique_lock<std::mutex> lock(runtime.mutex);
            runtime.condition.wait(lock, [&runtime]() {
                return runtime.stopRequested || !runtime.queue.empty();
            });

            if (runtime.stopRequested)
                return;

            if (runtime.queue.empty())
                continue;

            job = runtime.queue.front();
            runtime.queue.pop_front();
        }

        if (!job)
            continue;

        if (job->cancelRequested.load())
        {
            std::lock_guard<std::mutex> guard(m_jobsMutex);
            job->state = IppJobCanceled;
            job->stateReasons = "job-canceled-by-user";
            job->completedUpTime = UpTime();
            RemoveJobFiles(job);
            continue;
        }

        {
            std::lock_guard<std::mutex> guard(m_jobsMutex);
            if (job->state == IppJobPending)
            {
                job->state = IppJobProcessing;
                job->stateReasons = "job-processing";
                job->processingUpTime = UpTime();
            }
        }

        runtime.currentJobId.store(job->id);

        bool canceled = false;
        bool ok = false;
        std::string printError;

        if (job->tempPathUtf8.empty() || job->documentBytes <= 0)
        {
            printError = "作业文档为空";
        }
        else
        {
            ok = PrintJobDocument(job, printerIndex, canceled, printError);
        }

        runtime.currentJobId.store(0);

        {
            std::lock_guard<std::mutex> guard(m_jobsMutex);
            if (canceled || job->cancelRequested.load())
            {
                job->state = IppJobCanceled;
                job->stateReasons = "job-canceled-by-user";
            }
            else if (ok)
            {
                job->state = IppJobCompleted;
                job->stateReasons = "job-completed-successfully";
            }
            else
            {
                job->state = IppJobAborted;
                job->stateReasons = "aborted-by-system";
            }
            job->completedUpTime = UpTime();
        }

        Log("作业 #" + std::to_string(job->id) + " 结束: " +
            (canceled ? "已取消" : (ok ? "完成" : ("失败 - " + printError))));

        RemoveJobFiles(job);
    }
}

} // namespace ps
