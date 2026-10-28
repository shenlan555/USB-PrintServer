#pragma once

// 轻量 HTTP 客户端（WinHTTP）
//
// 目的：替代 C# 的 System.Net.Http.HttpClient 在本项目中的全部用法：
//   - TestConnectionAsync : GetAsync(base + "/api/test")            超时 5 秒
//   - GetPrinterInfoAsync : GetAsync(base + "/api/info")            超时 10 秒
//   - DownloadDriverAsync : GetAsync(..., ResponseHeadersRead)      超时 5 分钟
//                          按 8192 字节分块写入文件并汇报百分比
//
// 错误消息刻意沿用 .NET 的英文资源串（HttpRequestException.Message），
// 以便 "获取打印机信息失败: " 之后的文本与 C# 版本一致。

#include <string>
#include <functional>
#include <utility>
#include <vector>

namespace ps
{
namespace Http
{

// 附加请求头（名称: 值）
using HeaderList = std::vector<std::pair<std::string, std::string>>;

struct Response
{
    int statusCode = 0;         // HTTP 状态码
    std::string statusText;     // 状态描述（对应 HttpResponseMessage.ReasonPhrase）
    std::string body;           // 响应正文（UTF-8）
    long long contentLength = -1; // Content-Length 头；缺失为 -1
    bool received = false;      // 是否收到了响应（false 表示连接/发送阶段失败）
};

// 是否 2xx（对应 HttpResponseMessage.IsSuccessStatusCode）
bool IsSuccess(const Response& response);

// 对应 HttpResponseMessage.EnsureSuccessStatusCode() 抛出的异常消息：
// "Response status code does not indicate success: 404 (Not Found)."
std::string EnsureSuccessMessage(const Response& response);

// 简单 GET（整体读取响应正文）
bool Get(const std::wstring& url, int timeoutMs, Response& response, std::string& error,
         const HeaderList& extraHeaders = HeaderList());

// 流式 GET：每收到一块数据调用 onChunk(data, size, received, total)，
// onChunk 返回 false 可中止下载
using ChunkCallback = std::function<bool(const char* data, size_t size,
                                         long long received, long long total)>;

bool Download(const std::wstring& url, int timeoutMs, const ChunkCallback& onChunk,
              Response& response, std::string& error,
              const HeaderList& extraHeaders = HeaderList());

// 把 WinHTTP 失败码翻译成 .NET HttpRequestException 的英文消息
std::string DescribeLastError(unsigned long errorCode);

} // namespace Http
} // namespace ps
