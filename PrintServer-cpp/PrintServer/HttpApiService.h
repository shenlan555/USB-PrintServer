#pragma once

// 内置 HTTP API 服务（对应 C# HttpApiService.cs）
//
// 为客户端提供打印机列表查询与驱动文件下载，支持多打印机。
// 使用 Windows HTTP Server API（httpapi.dll）实现，与 C# 的 HttpListener 同源，
// 因此监听前缀、URL ACL 权限语义、错误行为都保持一致。

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include "PrintService.h"

namespace ps
{

class HttpApiServer
{
public:
    HttpApiServer(const std::vector<PrinterConfig>& printers, int port);
    ~HttpApiServer();

    HttpApiServer(const HttpApiServer&) = delete;
    HttpApiServer& operator=(const HttpApiServer&) = delete;

    void Start();
    void Stop();

    bool IsRunning() const { return m_running.load(); }

    // 对应 C# 的 OnLog 事件
    std::function<void(const std::string&)> OnLog;

private:
    void ListenLoop();
    void HandleRequest(const void* requestPtr, const std::string& body,
                       int& statusCode, std::string& contentType,
                       std::vector<std::pair<std::string, std::string>>& headers,
                       std::vector<unsigned char>& responseBody);

    std::string BuildInfoJson();
    std::string BuildTestJson();
    bool BuildDriverZip(const std::string& queryStringUtf8,
                        std::vector<unsigned char>& zip,
                        std::string& downloadFileName);

    void Log(const std::string& message);

    std::vector<PrinterConfig> m_printers;
    int m_port;
    std::atomic<bool> m_running;
    std::thread m_listenThread;
    void* m_queue;      // HANDLE
};

}
