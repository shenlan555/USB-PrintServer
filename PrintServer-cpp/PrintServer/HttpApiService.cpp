#include "pch.h"
#include "HttpApiService.h"
#include "Crypto.h"
#include "DriverPackager.h"
#include "Json.h"
#include "Wmi.h"
#include "BuildConfig.h"

#include <http.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctime>

#pragma comment(lib, "httpapi.lib")

namespace ps
{

namespace
{

std::string NowText()
{
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char buf[32] = { 0 };
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
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

    char buf[64] = { 0 };
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.2f", size);

    std::string text = buf;
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();

    return text + " " + sizes[order];
}

std::string ToLowerAsciiCopy(const std::string& text)
{
    std::string out = text;
    for (char& c : out)
    {
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }
    return out;
}

// 替换文件名中的非法字符（对应 C# Path.GetInvalidFileNameChars）
std::string SanitizeFileName(const std::string& name)
{
    static const char* kInvalid = "<>:\"/\\|?*";
    std::string out = name;
    for (char& c : out)
    {
        if (strchr(kInvalid, c) != nullptr || (unsigned char)c < 0x20)
            c = '_';
    }
    return out;
}

// 从查询串中取参数（对应 C# QueryString["id"]）
std::string GetQueryParam(const std::string& query, const std::string& key)
{
    if (query.empty())
        return std::string();

    std::string needle = key + "=";
    size_t pos = 0;

    // HTTP.sys 的 CookedUrl.QueryString 带前导 '?'（例如 "?id=1"），
    // 而 .NET 的 Request.QueryString 会自动去掉；这里必须显式跳过，
    // 否则 "?id=1" 匹配不上 "id="，索引永远回落到 0（下载到第一台打印机的驱动）。
    if (query[0] == '?')
        pos = 1;

    while (pos < query.size())
    {
        size_t end = query.find('&', pos);
        std::string item = (end == std::string::npos)
            ? query.substr(pos)
            : query.substr(pos, end - pos);

        if (item.compare(0, needle.size(), needle) == 0)
            return item.substr(needle.size());

        if (end == std::string::npos)
            break;
        pos = end + 1;
    }

    return std::string();
}

bool FileExists(const std::string& utf8Path)
{
    return ::GetFileAttributesW(Utf8ToWide(utf8Path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 取自定义请求头（HTTP.sys 把非标准头放在 pUnknownHeaders 里）
std::string GetRequestHeader(PHTTP_REQUEST request, const char* name)
{
    if (request == nullptr || name == nullptr)
        return std::string();

    size_t nameLength = strlen(name);

    for (USHORT i = 0; i < request->Headers.UnknownHeaderCount; ++i)
    {
        const HTTP_UNKNOWN_HEADER& header = request->Headers.pUnknownHeaders[i];
        if (header.pName == nullptr || header.pRawValue == nullptr)
            continue;
        if ((size_t)header.NameLength != nameLength)
            continue;
        if (_strnicmp(header.pName, name, nameLength) != 0)
            continue;

        std::string value(header.pRawValue, header.RawValueLength);

        // 去掉首尾空白
        size_t begin = 0;
        size_t end = value.size();
        while (begin < end && (value[begin] == ' ' || value[begin] == '\t'))
            ++begin;
        while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                               value[end - 1] == '\r' || value[end - 1] == '\n'))
        {
            --end;
        }
        return value.substr(begin, end - begin);
    }

    return std::string();
}

// /api/driver 鉴权：时间戳窗口 + HMAC-SHA256。
// 说明：这是对称共享密钥，仅用于挡掉局域网上的随意抓取/扫描，
//       驱动包的防篡改由清单的 RSA 签名负责。
bool ValidateDriverAuth(PHTTP_REQUEST request, const std::string& path,
                        const std::string& query, std::string& reason)
{
    std::string timestamp = GetRequestHeader(request, "X-PS-Time");
    std::string signature = GetRequestHeader(request, "X-PS-Auth");

    if (timestamp.empty() || signature.empty())
    {
        reason = "缺少鉴权头";
        return false;
    }

    long long clientTime = _strtoi64(timestamp.c_str(), nullptr, 10);
    if (clientTime <= 0)
    {
        reason = "时间戳无效";
        return false;
    }

    long long now = (long long)::time(nullptr);
    long long skew = (clientTime > now) ? (clientTime - now) : (now - clientTime);
    if (skew > (long long)BuildConfig::ApiAuthMaxSkewSeconds)
    {
        reason = "时间戳超出允许范围（时钟不同步或重放）";
        return false;
    }

    // 签名对象："<时间戳>|<路径+查询串>"，与客户端 BuildAuthHeaders 一致
    std::string data = timestamp + "|" + path + query;
    if (!Crypto::VerifyHmacSignature(data, signature, BuildConfig::ApiAuthSecret))
    {
        reason = "HMAC 校验失败";
        return false;
    }

    return true;
}

// HTTP 未知头为 ANSI 编码，非 ASCII 文件名需替换为 '_' 作为回退
std::string AsciiFallback(const std::string& utf8Text)
{
    std::string out;
    out.reserve(utf8Text.size());
    for (unsigned char c : utf8Text)
        out.push_back((c < 0x80) ? (char)c : '_');
    return out;
}

// RFC 5987 百分号编码（UTF-8）
std::string PercentEncodeUtf8(const std::string& utf8Text)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(utf8Text.size() * 3);

    for (unsigned char c : utf8Text)
    {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                          c == '.' || c == '~';
        if (unreserved)
        {
            out.push_back((char)c);
        }
        else
        {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }

    return out;
}

} // namespace

HttpApiServer::HttpApiServer(const std::vector<PrinterConfig>& printers, int port)
    : m_printers(printers)
    , m_port(port)
    , m_running(false)
    , m_queue(nullptr)
{
}

HttpApiServer::~HttpApiServer()
{
    Stop();
}

void HttpApiServer::Log(const std::string& message)
{
    std::string line = "[" + NowText() + "] [HTTP-API] " + message;
    if (OnLog)
        OnLog(line);
}

void HttpApiServer::Start()
{
    HTTPAPI_VERSION version = HTTPAPI_VERSION_1;

    if (::HttpInitialize(version, HTTP_INITIALIZE_SERVER, nullptr) != NO_ERROR)
    {
        Log("HTTP API 初始化失败");
        return;
    }

    HANDLE queue = nullptr;
    if (::HttpCreateHttpHandle(&queue, 0) != NO_ERROR)
    {
        Log("HTTP API 创建请求队列失败");
        ::HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
        return;
    }

    m_queue = queue;

    // 与 C# 一致：先尝试 http://+:port/，失败则降级为仅本机
    wchar_t urlAll[MAX_PATH] = { 0 };
    _snwprintf_s(urlAll, _countof(urlAll), _TRUNCATE, L"http://+:%d/", m_port);

    ULONG result = ::HttpAddUrl(queue, urlAll, nullptr);

    if (result != NO_ERROR)
    {
        wchar_t urlLocal[MAX_PATH] = { 0 };
        _snwprintf_s(urlLocal, _countof(urlLocal), _TRUNCATE, L"http://localhost:%d/", m_port);
        result = ::HttpAddUrl(queue, urlLocal, nullptr);

        if (result != NO_ERROR)
        {
            char localhost2[MAX_PATH] = { 0 };
            _snprintf_s(localhost2, sizeof(localhost2), _TRUNCATE, "http://127.0.0.1:%d/", m_port);
            result = ::HttpAddUrl(queue, Utf8ToWide(localhost2).c_str(), nullptr);
        }

        if (result == NO_ERROR)
        {
            Log("HTTP API 已降级启动（仅本机），端口: " + std::to_string(m_port));
        }
        else
        {
            Log("HTTP API 启动失败: 无法注册 URL（错误 " + std::to_string(result) +
                "，端口 " + std::to_string(m_port) + " 可能需要管理员权限或 URL 预留）");
            ::CloseHandle(queue);
            m_queue = nullptr;
            ::HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
            return;
        }
    }
    else
    {
        Log("HTTP API 服务已启动，端口: " + std::to_string(m_port));
    }

    m_running.store(true);
    m_listenThread = std::thread([this]() { ListenLoop(); });
}

void HttpApiServer::Stop()
{
    if (!m_running.exchange(false))
        return;

    HANDLE queue = (HANDLE)m_queue;
    if (queue != nullptr)
    {
        // 关闭请求队列会让阻塞中的 HttpReceiveHttpRequest 立即返回
        // HttpCreateHttpHandle 返回的是内核句柄，用 CloseHandle 关闭
        ::CloseHandle(queue);
        m_queue = nullptr;
    }

    if (m_listenThread.joinable())
        m_listenThread.join();

    ::HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);

    Log("HTTP API 服务已停止");
}

void HttpApiServer::ListenLoop()
{
    std::vector<unsigned char> buffer(sizeof(HTTP_REQUEST) + 4096);

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
            buffer.resize(bytesReturned);
            continue;
        }

        if (result != NO_ERROR)
        {
            if (!m_running.load())
                break;
            Log("HTTP 监听异常（错误 " + std::to_string(result) + "）");
            continue;
        }

        PHTTP_REQUEST request = (PHTTP_REQUEST)buffer.data();

        // 读取请求体（若有）：Content-Length 需从请求头中查找
        std::string body;
        if ((request->Flags & HTTP_REQUEST_FLAG_MORE_ENTITY_BODY_EXISTS) != 0)
        {
            ULONG contentLength = 0;

            // 先看已知头 Content-Length
            {
                const HTTP_KNOWN_HEADER& known =
                    request->Headers.KnownHeaders[HttpHeaderContentLength];

                if (known.pRawValue != nullptr && known.RawValueLength > 0)
                {
                    contentLength = (ULONG)atoi(
                        std::string(known.pRawValue, known.RawValueLength).c_str());
                }
                else
                {
                    // 回退：扫描自定义头
                    for (USHORT i = 0; i < request->Headers.UnknownHeaderCount; ++i)
                    {
                        const HTTP_UNKNOWN_HEADER& uh = request->Headers.pUnknownHeaders[i];
                        if (uh.NameLength == 14 && uh.pName != nullptr &&
                            _strnicmp(uh.pName, "Content-Length", 14) == 0)
                        {
                            contentLength = (ULONG)atoi(
                                std::string(uh.pRawValue, uh.RawValueLength).c_str());
                            break;
                        }
                    }
                }
            }

            if (contentLength > 0)
            {
                body.resize(contentLength);
                ULONG offset = 0;
                ULONG read = 0;
                while (offset < contentLength)
                {
                    if (::HttpReceiveRequestEntityBody(queue, request->RequestId, 0,
                                                       &body[offset], contentLength - offset,
                                                       &read, nullptr) != NO_ERROR)
                    {
                        break;
                    }
                    if (read == 0)
                        break;
                    offset += read;
                }
                body.resize(offset);
            }
        }

        int statusCode = 404;
        std::string contentType = "application/json; charset=utf-8";
        std::vector<std::pair<std::string, std::string>> headers;
        std::vector<unsigned char> responseBody;

        HandleRequest(request, body, statusCode, contentType, headers, responseBody);

        // 构造响应
        HTTP_RESPONSE response;
        memset(&response, 0, sizeof(response));
        response.StatusCode = (USHORT)statusCode;
        response.pReason = (statusCode == 200) ? "OK"
                         : (statusCode == 404) ? "Not Found"
                         : (statusCode == 500) ? "Internal Server Error" : "Error";
        response.ReasonLength = (USHORT)strlen(response.pReason);

        // 用已知头承载 Content-Type
        response.Headers.KnownHeaders[HttpHeaderContentType].pRawValue = contentType.c_str();
        response.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength = (USHORT)contentType.size();

        // CORS 与其它自定义头
        std::vector<HTTP_UNKNOWN_HEADER> unknownHeaders;
        std::vector<std::string> headerNames, headerValues;

        headerNames.push_back("Access-Control-Allow-Origin");
        headerValues.push_back("*");
        headerNames.push_back("Access-Control-Allow-Methods");
        headerValues.push_back("GET, POST, OPTIONS");
        headerNames.push_back("Access-Control-Allow-Headers");
        headerValues.push_back("Content-Type, Authorization");

        for (const auto& kv : headers)
        {
            headerNames.push_back(kv.first);
            headerValues.push_back(kv.second);
        }

        unknownHeaders.resize(headerNames.size());
        for (size_t i = 0; i < headerNames.size(); ++i)
        {
            memset(&unknownHeaders[i], 0, sizeof(HTTP_UNKNOWN_HEADER));
            unknownHeaders[i].pName = headerNames[i].c_str();
            unknownHeaders[i].NameLength = (USHORT)headerNames[i].size();
            unknownHeaders[i].pRawValue = headerValues[i].c_str();
            unknownHeaders[i].RawValueLength = (USHORT)headerValues[i].size();
        }

        response.Headers.UnknownHeaderCount = (USHORT)unknownHeaders.size();
        response.Headers.pUnknownHeaders = unknownHeaders.empty() ? nullptr : unknownHeaders.data();

        HTTP_DATA_CHUNK dataChunk;
        memset(&dataChunk, 0, sizeof(dataChunk));

        if (!responseBody.empty())
        {
            dataChunk.DataChunkType = HttpDataChunkFromMemory;
            dataChunk.FromMemory.pBuffer = responseBody.data();
            dataChunk.FromMemory.BufferLength = (ULONG)responseBody.size();
            response.EntityChunkCount = 1;
            response.pEntityChunks = &dataChunk;
        }

        DWORD sent = 0;
        ::HttpSendHttpResponse(queue, request->RequestId, 0, &response, nullptr, &sent,
                               nullptr, 0, nullptr, nullptr);
    }
}

void HttpApiServer::HandleRequest(const void* requestPtr, const std::string& body,
                                  int& statusCode, std::string& contentType,
                                  std::vector<std::pair<std::string, std::string>>& headers,
                                  std::vector<unsigned char>& responseBody)
{
    PHTTP_REQUEST request = (PHTTP_REQUEST)requestPtr;
    UNREFERENCED_PARAMETER(body);

    // 请求方法
    std::string method;
    if (request->Verb == HttpVerbGET)          method = "GET";
    else if (request->Verb == HttpVerbPOST)    method = "POST";
    else if (request->Verb == HttpVerbOPTIONS) method = "OPTIONS";
    else                                       method = "OTHER";

    // 请求路径
    std::string path = WideToUtf8(std::wstring(
        request->CookedUrl.pAbsPath,
        request->CookedUrl.AbsPathLength / sizeof(wchar_t)));

    // 查询串
    std::string query;
    if (request->CookedUrl.pQueryString != nullptr && request->CookedUrl.QueryStringLength > 0)
    {
        query = WideToUtf8(std::wstring(request->CookedUrl.pQueryString,
                                        request->CookedUrl.QueryStringLength / sizeof(wchar_t)));
    }

    // 客户端 IP
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

    std::string pathLower = ToLowerAsciiCopy(path);

    Log("API请求: " + method + " " + path + " 来自 " + clientIp);

    if (method == "OPTIONS")
    {
        statusCode = 200;
        responseBody.clear();
        return;
    }

    if (pathLower == "/api/info")
    {
        std::string json = BuildInfoJson();
        responseBody.assign(json.begin(), json.end());
        statusCode = 200;
        return;
    }

    if (pathLower == "/api/driver")
    {
        // 驱动包下载需要鉴权（HMAC + 时间戳），失败直接 403
        std::string authReason;
        if (!ValidateDriverAuth(request, path, query, authReason))
        {
            Log("API拒绝: /api/driver 鉴权失败 (" + authReason + ") 来自 " + clientIp);
            std::string json = "{\"error\":\"未通过接口鉴权\"}";
            responseBody.assign(json.begin(), json.end());
            statusCode = 403;
            return;
        }

        std::vector<unsigned char> zip;
        std::string fileName;

        if (BuildDriverZip(query, zip, fileName))
        {
            responseBody = zip;
            statusCode = 200;
            contentType = "application/zip";

            // HTTP 未知头按 ANSI 处理，中文文件名按 RFC 5987 补充 filename*
            std::string disposition = "attachment; filename=\"" + AsciiFallback(fileName) +
                                      "\"; filename*=UTF-8''" + PercentEncodeUtf8(fileName);
            headers.push_back(std::make_pair("Content-Disposition", disposition));

            Log("驱动下载: " + fileName + " (" + FormatSize((long long)zip.size()) + ")");
            return;
        }

        std::string json = "{\"error\":\"无法获取打印机驱动文件\"}";
        responseBody.assign(json.begin(), json.end());
        statusCode = 404;
        return;
    }

    if (pathLower == "/api/test")
    {
        std::string json = BuildTestJson();
        responseBody.assign(json.begin(), json.end());
        statusCode = 200;
        return;
    }

    JsonValue err = JsonValue::Object();
    err.Set("error", JsonValue::Str("未知的API端点"));
    err.Set("path", JsonValue::Str(path));
    std::string json = err.Serialize();
    responseBody.assign(json.begin(), json.end());
    statusCode = 404;
}

std::string HttpApiServer::BuildInfoJson()
{
    JsonValue printers = JsonValue::Array();

    // 每台打印机只解析一次驱动包，兼容字段复用同一份结果
    std::vector<PrinterDriverInfo> infos;
    infos.reserve(m_printers.size());

    for (const auto& cfg : m_printers)
    {
        infos.push_back(DriverPackager::GetPrinterDriverInfo(cfg.name));
        const PrinterDriverInfo& info = infos.back();

        JsonValue item = JsonValue::Object();
        item.Set("index", JsonValue::Int(cfg.index));
        item.Set("name", JsonValue::Str(cfg.name));
        item.Set("port", JsonValue::Int(cfg.port));
        item.Set("driverName", JsonValue::Str(info.driverName));
        item.Set("driverVersion", JsonValue::Str(info.driverVersion));
        item.Set("driverDate", JsonValue::Str(info.driverDate));
        item.Set("manufacturer", JsonValue::Str(info.manufacturer));
        item.Set("hasDriver", JsonValue::Bool(info.hasDriverFiles));
        item.Set("driverFileCount", JsonValue::Int(info.driverFileCount));
        item.Set("driverSizeBytes", JsonValue::Int(info.driverPackageSize));
        item.Set("driverSizeFormatted", JsonValue::Str(FormatSize(info.driverPackageSize)));
        // v1.4.1 新增：驱动包来源与完整性，便于客户端/排障判断
        item.Set("driverInfName", JsonValue::Str(info.infPath));
        item.Set("driverArchitecture", JsonValue::Str(info.architecture));
        item.Set("driverSource", JsonValue::Str(info.packageSource));
        item.Set("driverComplete", JsonValue::Bool(info.complete));
        item.Set("driverMissingCount", JsonValue::Int(info.missingFileCount));

        printers.arr.push_back(item);
    }

    JsonValue server = JsonValue::Object();
    server.Set("hostname", JsonValue::Str([]() {
        wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
        DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
        ::GetComputerNameW(buf, &len);
        return WideToUtf8(std::wstring(buf, len));
    }()));
    server.Set("version", JsonValue::Str(BuildConfig::VersionA));
    server.Set("apiPort", JsonValue::Int(m_port));
    server.Set("printerCount", JsonValue::Int((long long)m_printers.size()));

    JsonValue root = JsonValue::Object();
    root.Set("server", server);

    // 兼容旧版：第一台打印机信息
    if (!m_printers.empty() && !infos.empty())
    {
        const PrinterDriverInfo& first = infos[0];

        JsonValue compat = JsonValue::Object();
        compat.Set("name", JsonValue::Str(m_printers[0].name));
        compat.Set("tcpPort", JsonValue::Int(m_printers[0].port));
        compat.Set("driverName", JsonValue::Str(first.driverName));
        compat.Set("driverVersion", JsonValue::Str(first.driverVersion));
        compat.Set("driverDate", JsonValue::Str(first.driverDate));
        compat.Set("manufacturer", JsonValue::Str(first.manufacturer));
        compat.Set("hasDriver", JsonValue::Bool(first.hasDriverFiles));
        compat.Set("driverFileCount", JsonValue::Int(first.driverFileCount));
        compat.Set("driverSizeBytes", JsonValue::Int(first.driverPackageSize));
        compat.Set("driverSizeFormatted", JsonValue::Str(FormatSize(first.driverPackageSize)));
        compat.Set("driverInfName", JsonValue::Str(first.infPath));
        compat.Set("driverArchitecture", JsonValue::Str(first.architecture));
        compat.Set("driverSource", JsonValue::Str(first.packageSource));
        compat.Set("driverComplete", JsonValue::Bool(first.complete));
        compat.Set("driverMissingCount", JsonValue::Int(first.missingFileCount));

        root.Set("printer", compat);
    }
    else
    {
        root.Set("printer", JsonValue());
    }

    root.Set("printers", printers);
    return root.Serialize();
}

std::string HttpApiServer::BuildTestJson()
{
    JsonValue names = JsonValue::Array();
    for (const auto& cfg : m_printers)
        names.arr.push_back(JsonValue::Str(cfg.name));

    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char now[32] = { 0 };
    _snprintf_s(now, sizeof(now), _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    JsonValue root = JsonValue::Object();
    root.Set("status", JsonValue::Str("ok"));
    root.Set("serverTime", JsonValue::Str(now));
    root.Set("printerCount", JsonValue::Int((long long)m_printers.size()));
    root.Set("printerNames", names);
    return root.Serialize();
}

bool HttpApiServer::BuildDriverZip(const std::string& queryStringUtf8,
                                   std::vector<unsigned char>& zip,
                                   std::string& downloadFileName)
{
    if (m_printers.empty())
        return false;

    int requestedId = 0;
    std::string idParam = GetQueryParam(queryStringUtf8, "id");
    if (!idParam.empty())
        requestedId = atoi(idParam.c_str());

    // id 是配置里的"稳定槽位"（PrinterConfig::index），不是数组下标：
    // 槽位允许有空洞（中间某台打印机未勾选），直接当下标用会下载到错误的驱动包。
    int printerIndex = -1;
    for (size_t i = 0; i < m_printers.size(); ++i)
    {
        if (m_printers[i].index == requestedId)
        {
            printerIndex = (int)i;
            break;
        }
    }

    if (printerIndex < 0)
        printerIndex = 0;   // id 缺失/非法：退回第一台（保持旧客户端兼容）

    const std::string& printerName = m_printers[printerIndex].name;

    zip = DriverPackager::PackageDriverFiles(printerName);
    if (zip.empty())
        return false;

    downloadFileName = SanitizeFileName(printerName) + "_driver.zip";
    return true;
}

}
