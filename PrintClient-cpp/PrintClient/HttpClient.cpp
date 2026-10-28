#include "pch.h"
#include "HttpClient.h"

#include <string>
#include <vector>

#include <winhttp.h>

#include "Wmi.h"    // ps::WideToUtf8（复用 PrintServer-cpp 的公共实现）

namespace ps
{
namespace Http
{

namespace
{

const wchar_t* kUserAgent = L"PrintClient/1.0";

// 组装的 URL 各段
struct CrackedUrl
{
    std::wstring host;
    std::wstring target;    // path + query
    INTERNET_PORT port = 0;
};

bool CrackUrl(const std::wstring& url, CrackedUrl& cracked)
{
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);

    wchar_t host[512] = { 0 };
    wchar_t path[4096] = { 0 };
    wchar_t extra[4096] = { 0 };

    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = _countof(extra);

    if (!::WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc))
        return false;

    cracked.host = host;
    cracked.target = path;
    cracked.target += extra;
    cracked.port = uc.nPort;

    if (cracked.port == 0)
        cracked.port = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? 443 : 80;

    return !cracked.host.empty();
}

// 处理一次请求的公共流程；onChunk 为空表示整体读取到 body
bool PerformRequest(const std::wstring& url, int timeoutMs, const ChunkCallback* onChunk,
                    Response& response, std::string& error,
                    const HeaderList& extraHeaders)
{
    response = Response();

    CrackedUrl cracked;
    if (!CrackUrl(url, cracked))
    {
        error = "Invalid URL";
        return false;
    }

    HINTERNET session = ::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
    {
        error = DescribeLastError(::GetLastError());
        return false;
    }

    // 对应 HttpClient.Timeout：各阶段都限制在同一时长内
    ::WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET connect = ::WinHttpConnect(session, cracked.host.c_str(), cracked.port, 0);
    if (connect == nullptr)
    {
        error = DescribeLastError(::GetLastError());
        ::WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = ::WinHttpOpenRequest(connect, L"GET", cracked.target.c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (request == nullptr)
    {
        error = DescribeLastError(::GetLastError());
        ::WinHttpCloseHandle(connect);
        ::WinHttpCloseHandle(session);
        return false;
    }

    // 对应 HttpClientHandler 的 AllowAutoRedirect = true（默认）
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    ::WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                       sizeof(redirectPolicy));

    // 附加请求头（/api/driver 鉴权用）
    if (!extraHeaders.empty())
    {
        std::string combined;
        for (const auto& header : extraHeaders)
        {
            combined += header.first;
            combined += ": ";
            combined += header.second;
            combined += "\r\n";
        }

        std::wstring wideHeaders = Utf8ToWide(combined);
        ::WinHttpAddRequestHeaders(request, wideHeaders.c_str(), (DWORD)-1L,
                                   WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    bool ok = false;
    bool reachedResponse = false;

    if (!::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        error = DescribeLastError(::GetLastError());
    }
    else if (!::WinHttpReceiveResponse(request, nullptr))
    {
        error = DescribeLastError(::GetLastError());
    }
    else
    {
        reachedResponse = true;

        DWORD code = 0;
        DWORD len = sizeof(code);
        if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX))
        {
            response.statusCode = (int)code;
        }

        wchar_t reason[256] = { 0 };
        DWORD reasonLen = sizeof(reason);
        if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_TEXT, WINHTTP_HEADER_NAME_BY_INDEX,
                                  reason, &reasonLen, WINHTTP_NO_HEADER_INDEX))
        {
            response.statusText = WideToUtf8(reason);
        }

        wchar_t lengthText[64] = { 0 };
        DWORD lengthTextLen = sizeof(lengthText);
        if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                  lengthText, &lengthTextLen, WINHTTP_NO_HEADER_INDEX))
        {
            response.contentLength = _wtoi64(lengthText);
        }

        long long received = 0;
        ok = true;

        for (;;)
        {
            DWORD available = 0;
            if (!::WinHttpQueryDataAvailable(request, &available))
            {
                error = DescribeLastError(::GetLastError());
                ok = false;
                break;
            }

            if (available == 0)
                break;

            std::vector<char> chunk(available);
            DWORD read = 0;
            if (!::WinHttpReadData(request, chunk.data(), available, &read))
            {
                error = DescribeLastError(::GetLastError());
                ok = false;
                break;
            }

            if (read == 0)
                break;

            received += (long long)read;

            if (onChunk != nullptr)
            {
                if (!(*onChunk)(chunk.data(), (size_t)read, received, response.contentLength))
                    break;
            }
            else
            {
                response.body.append(chunk.data(), read);
            }
        }
    }

    response.received = reachedResponse;

    ::WinHttpCloseHandle(request);
    ::WinHttpCloseHandle(connect);
    ::WinHttpCloseHandle(session);

    return ok;
}

} // namespace

bool IsSuccess(const Response& response)
{
    return response.statusCode >= 200 && response.statusCode <= 299;
}

std::string EnsureSuccessMessage(const Response& response)
{
    char buffer[256] = { 0 };
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                "Response status code does not indicate success: %d (%s).",
                response.statusCode, response.statusText.c_str());
    return buffer;
}

std::string DescribeLastError(unsigned long errorCode)
{
    // 对应 .NET Framework 的 HttpRequestException / WebException 英文消息
    switch (errorCode)
    {
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_SECURE_FAILURE:
        return "Unable to connect to the remote server";
    case ERROR_WINHTTP_TIMEOUT:
        return "The operation has timed out";
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return "The underlying connection was closed: An unexpected error occurred on a send.";
    default:
        break;
    }

    char buffer[128] = { 0 };
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                "An error occurred while sending the request. (WinHTTP error %lu)", errorCode);
    return buffer;
}

bool Get(const std::wstring& url, int timeoutMs, Response& response, std::string& error,
         const HeaderList& extraHeaders)
{
    error.clear();
    return PerformRequest(url, timeoutMs, nullptr, response, error, extraHeaders);
}

bool Download(const std::wstring& url, int timeoutMs, const ChunkCallback& onChunk,
              Response& response, std::string& error,
              const HeaderList& extraHeaders)
{
    error.clear();
    return PerformRequest(url, timeoutMs, &onChunk, response, error, extraHeaders);
}

} // namespace Http
} // namespace ps
