#include "pch.h"
#include "CryptoSelfTest.h"
#include "Crypto.h"
#include "BuildConfig.h"
#include "Config.h"
#include "Json.h"
#include "PrintService.h"
#include "HttpApiService.h"
#include "ServiceHost.h"
#include "LogHelper.h"

#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mutex>
#include <thread>
#include <chrono>

#include <string>
#include <vector>
#include <map>
#ifdef _DEBUG

#pragma comment(lib, "shell32.lib")

namespace ps
{

namespace
{

std::vector<char> ReadAllBytes(const std::wstring& path)
{
    std::vector<char> empty;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return empty;

    LARGE_INTEGER size = { 0 };
    if (!::GetFileSizeEx(h, &size))
    {
        ::CloseHandle(h);
        return empty;
    }

    std::vector<char> buf((size_t)size.QuadPart);
    DWORD read = 0;
    if (!buf.empty())
        ::ReadFile(h, buf.data(), (DWORD)buf.size(), &read, nullptr);
    ::CloseHandle(h);

    buf.resize(read);
    return buf;
}

bool WriteAllBytes(const std::wstring& path, const std::vector<char>& data)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    bool ok = data.empty() ||
              (::WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) != 0);
    ::CloseHandle(h);
    return ok;
}

std::map<std::string, std::string> ParseVectors(const std::vector<char>& raw)
{
    std::map<std::string, std::string> result;
    std::string text(raw.begin(), raw.end());

    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t eol = text.find('\n', pos);
        std::string line = (eol == std::string::npos)
            ? text.substr(pos)
            : text.substr(pos, eol - pos);

        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0)
            result[line.substr(0, eq)] = line.substr(eq + 1); // 只按第一个 '=' 切分（base64 含 '='）

        if (eol == std::string::npos)
            break;
        pos = eol + 1;
    }

    return result;
}

std::string ToHexLower(const std::vector<unsigned char>& data)
{
    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.reserve(data.size() * 2);
    for (unsigned char c : data)
    {
        s.push_back(kHex[c >> 4]);
        s.push_back(kHex[c & 0x0F]);
    }
    return s;
}

std::string Get(const std::map<std::string, std::string>& m, const std::string& key)
{
    auto it = m.find(key);
    return (it == m.end()) ? std::string() : it->second;
}

} // namespace

int RunCryptoSelfTest(const std::wstring& vectorFilePath, const std::wstring& reportPath)
{
    std::vector<char> raw = ReadAllBytes(vectorFilePath);
    std::string report;
    int failed = 0;

    auto append = [&report](const char* fmt, ...) {
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
        va_end(args);
        report += buf;
    };

    auto check = [&](const char* name, const std::string& cppValue, const std::string& csValue) {
        bool pass = (cppValue == csValue);
        if (!pass)
            ++failed;
        append("[%s] %-28s\r\n", pass ? "PASS" : "FAIL", name);
        if (!pass)
        {
            append("        C++ : %.100s\r\n", cppValue.c_str());
            append("        C#  : %.100s\r\n", csValue.c_str());
        }
    };

    if (raw.empty())
    {
        append("[FAIL] 无法读取测试向量文件\r\n");
        ++failed;
    }
    else
    {
        std::map<std::string, std::string> v = ParseVectors(raw);

        const std::string md5Input    = Get(v, "md5_input");
        const std::string md5Expected = Get(v, "md5_hex_cs");
        const std::string hmacData    = Get(v, "hmac_data");
        const std::string hmacExpected= Get(v, "hmac_b64_cs");

        append("=========== PrintServer 加密自检 ===========\r\n\r\n");

        // 摘要：期望值由独立实现（.NET）算出，确认算法口径一致
        check("MD5", ToHexLower(Crypto::Md5(md5Input)), md5Expected);

        // HMAC-SHA256：与 /api/driver 鉴权使用的是同一组密钥与算法
        check("HMAC-SHA256(/api/driver 密钥)",
              Crypto::GenerateHmacSignature(hmacData, BuildConfig::ApiAuthSecret), hmacExpected);

        // 验签（大小写不敏感）
        const bool verifyOk = Crypto::VerifyHmacSignature(hmacData, hmacExpected, BuildConfig::ApiAuthSecret);
        const bool verifyBad = Crypto::VerifyHmacSignature(hmacData, "AAAA", BuildConfig::ApiAuthSecret);
        check("HMAC 验签(正例)", verifyOk ? "true" : "false", "true");
        check("HMAC 验签(反例)", verifyBad ? "true" : "false", "false");

        // Base64 往返
        {
            const std::string text = "PrintServer/1.4.3/base64";
            std::vector<unsigned char> bytes;
            const std::string encoded = Crypto::Base64Encode(
                std::vector<unsigned char>(text.begin(), text.end()));
            const bool decoded = Crypto::Base64Decode(encoded, bytes);
            check("Base64 往返",
                  (decoded && std::string(bytes.begin(), bytes.end()) == text) ? "ok" : "fail", "ok");
        }

        // ---------------- App.config 读取 ----------------
        append("\r\n---- 配置读取（App.config）----\r\n");
        check("PrinterCount", Config::GetSetting("PrinterCount", "(缺失)"), "0");
        check("LogRetentionDays", Config::GetSetting("LogRetentionDays", "(缺失)"), "15");
        check("ApiPort", Config::GetSetting("ApiPort", "(缺失)"), "9180");

        // 关键：被注释掉的 PrinterN_* 示例值绝不能被读出来
        check("注释项 Printer0_Name", Config::GetSetting("Printer0_Name", "(默认值)"), "(默认值)");
        check("注释项 Printer1_Name", Config::GetSetting("Printer1_Name", "(默认值)"), "(默认值)");
        check("不存在的键", Config::GetSetting("NoSuchKey", "(默认值)"), "(默认值)");
    }

    append("\r\n=========== 结果: %s (失败 %d 项) ===========\r\n",
           failed == 0 ? "全部通过" : "存在失败", failed);

    // 带 BOM 写入，便于记事本正确显示中文
    std::vector<char> out;
    const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    out.push_back((char)bom[0]);
    out.push_back((char)bom[1]);
    out.push_back((char)bom[2]);
    out.insert(out.end(), report.begin(), report.end());
    WriteAllBytes(reportPath, out);

    return failed;
}

// ===========================================================================
// TCP 打印服务端到端自测
// ===========================================================================

namespace
{

// 作为客户端连接并发送数据，然后半关闭以通知服务端"数据发完"
bool SendRawToPort(const std::string& host, int port, size_t bytes, size_t& sentBytes)
{
    sentBytes = 0;

    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    std::string portText = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0 || result == nullptr)
        return false;

    bool ok = false;
    SOCKET s = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (s != INVALID_SOCKET)
    {
        if (::connect(s, result->ai_addr, (int)result->ai_addrlen) == 0)
        {
            std::vector<char> payload(bytes, 'A');
            int sent = ::send(s, payload.data(), (int)payload.size(), 0);
            sentBytes = (sent > 0) ? (size_t)sent : 0;

            ::shutdown(s, SD_SEND);

            char drain[64];
            while (::recv(s, drain, sizeof(drain), 0) > 0)
            {
                // 服务端随后关闭连接
            }

            ok = (sent == (int)bytes);
        }
        ::closesocket(s);
    }

    ::freeaddrinfo(result);
    return ok;
}

}

int RunTcpSelfTest(const std::wstring& reportPath)
{
    std::string report;
    int failed = 0;

    auto append = [&report](const char* fmt, ...) {
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
        va_end(args);
        report += buf;
    };

    auto check = [&](const char* name, bool pass) {
        if (!pass)
            ++failed;
        append("[%s] %s\r\n", pass ? "PASS" : "FAIL", name);
    };

    std::vector<std::string> logs;
    std::mutex logMutex;

    const int port = 19100;
    const std::string printerName = "PS_TEST_PRINTER_不存在";

    TcpPrintServer server(printerName, port);
    server.OnLog = [&](const std::string& line) {
        std::lock_guard<std::mutex> guard(logMutex);
        logs.push_back(line);
    };

    append("=========== TCP 打印服务端到端自测 ===========\r\n\r\n");
    append("监听端口: %d\r\n打印机名: %s （故意不存在，用于验证失败路径）\r\n\r\n",
           port, printerName.c_str());

    bool started = false;
    try
    {
        server.Start();
        started = server.IsRunning();
    }
    catch (const std::exception& ex)
    {
        append("启动异常: %s\r\n", ex.what());
    }

    check("服务启动（双栈监听）", started);

    if (started)
    {
        size_t sent4 = 0;
        bool ipv4ok = SendRawToPort("127.0.0.1", port, 4096, sent4);
        check("IPv4 连接并发送 4096 字节", ipv4ok && sent4 == 4096);

        size_t sent6 = 0;
        bool ipv6ok = SendRawToPort("::1", port, 256, sent6);
        check("IPv6 连接并发送 256 字节", ipv6ok && sent6 == 256);

        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        server.Stop();
    }

    std::string all;
    {
        std::lock_guard<std::mutex> guard(logMutex);
        for (const auto& line : logs)
            all += line + "\n";
    }

    auto contains = [&all](const char* needle) {
        return all.find(needle) != std::string::npos;
    };

    check("日志含“已启动”", contains("已启动"));
    check("日志含“收到连接”", contains("收到连接"));
    check("日志含“接收数据: 4 KB”", contains("接收数据: 4 KB"));
    check("日志含“接收数据: 256 B”", contains("接收数据: 256 B"));
    check("RAW 打印已尝试并优雅失败", contains("打印失败"));
    check("服务已停止", contains("打印服务器已停止"));

    append("\r\n---- 服务日志（前 20 行）----\r\n");
    int shown = 0;
    for (const auto& line : logs)
    {
        if (shown++ >= 20)
            break;
        append("%s\r\n", line.c_str());
    }

    append("\r\n=========== 结果: %s (失败 %d 项) ===========\r\n",
           failed == 0 ? "全部通过" : "存在失败", failed);

    std::vector<char> out;
    const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    out.push_back((char)bom[0]);
    out.push_back((char)bom[1]);
    out.push_back((char)bom[2]);
    out.insert(out.end(), report.begin(), report.end());
    WriteAllBytes(reportPath, out);

    return failed;
}

// ===========================================================================
// HTTP API 端到端自测
// ===========================================================================

namespace
{

// 简单 HTTP 请求（WinHTTP）；返回 false 表示未收到响应
bool HttpRequest(const std::wstring& method, const std::wstring& url,
                 std::string& body, int& statusCode)
{
    body.clear();
    statusCode = 0;

    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);

    wchar_t host[256] = { 0 };
    wchar_t path[2048] = { 0 };
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);

    if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
        return false;

    HINTERNET session = ::WinHttpOpen(L"PrintServerSelfTest/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
        return false;

    HINTERNET connect = ::WinHttpConnect(session, host, uc.nPort, 0);
    if (connect == nullptr)
    {
        ::WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = ::WinHttpOpenRequest(connect, method.c_str(), path, nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (request == nullptr)
    {
        ::WinHttpCloseHandle(connect);
        ::WinHttpCloseHandle(session);
        return false;
    }

    DWORD timeout = 60000;
    ::WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    bool ok = false;
    if (::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        ::WinHttpReceiveResponse(request, nullptr))
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

        ok = true;
    }

    ::WinHttpCloseHandle(request);
    ::WinHttpCloseHandle(connect);
    ::WinHttpCloseHandle(session);
    return ok;
}

}

int RunApiSelfTest(const std::wstring& reportPath)
{
    std::string report;
    int failed = 0;

    auto append = [&report](const char* fmt, ...) {
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
        va_end(args);
        report += buf;
    };

    auto check = [&](const char* name, bool pass, const std::string& detail = std::string()) {
        if (!pass)
            ++failed;
        append("[%s] %s%s%s\r\n", pass ? "PASS" : "FAIL", name,
               detail.empty() ? "" : " -> ", detail.c_str());
    };

    const int port = 19180;

    std::vector<PrinterConfig> printers;
    {
        PrinterConfig cfg;
        cfg.name = "PS_API_TEST_PRINTER";
        cfg.port = 9101;
        cfg.enabled = true;
        cfg.index = 0;
        printers.push_back(cfg);
    }

    std::vector<std::string> logs;
    std::mutex logMutex;

    HttpApiServer server(printers, port);
    server.OnLog = [&](const std::string& line) {
        std::lock_guard<std::mutex> guard(logMutex);
        logs.push_back(line);
    };

    append("=========== HTTP API 端到端自测 ===========\r\n\r\n");
    append("端口: %d\r\n打印机: %s\r\n\r\n", port, printers[0].name.c_str());

    server.Start();
    bool started = server.IsRunning();
    check("服务启动", started);

    if (started)
    {
        const std::wstring base = L"http://localhost:" + std::to_wstring(port);

        // ---- /api/test ----
        std::string body;
        int status = 0;
        bool got = HttpRequest(L"GET", base + L"/api/test", body, status);
        check("GET /api/test 收到响应", got, "HTTP " + std::to_string(status));

        JsonValue json = JsonValue::Parse(body);
        const JsonValue* statusNode = json.Find("status");
        check("status == ok", statusNode != nullptr && statusNode->ToConvertString() == "ok");

        const JsonValue* countNode = json.Find("printerCount");
        check("printerCount == 1", countNode != nullptr && countNode->AsInt() == 1);

        const JsonValue* namesNode = json.Find("printerNames");
        bool nameOk = namesNode != nullptr && namesNode->type == JsonValue::ARR &&
                      namesNode->arr.size() == 1 &&
                      namesNode->arr[0].ToConvertString() == printers[0].name;
        check("printerNames[0] 正确", nameOk);

        // ---- /api/info ----
        body.clear();
        got = HttpRequest(L"GET", base + L"/api/info", body, status);
        check("GET /api/info 收到响应", got, "HTTP " + std::to_string(status));

        JsonValue info = JsonValue::Parse(body);
        const JsonValue* serverNode = info.Find("server");
        check("含 server 节点", serverNode != nullptr && serverNode->type == JsonValue::OBJ);

        const JsonValue* printerArr = info.Find("printers");
        check("printers 数组长度为 1",
              printerArr != nullptr && printerArr->type == JsonValue::ARR && printerArr->arr.size() == 1);

        bool infoNameOk = printerArr != nullptr && printerArr->type == JsonValue::ARR &&
                          !printerArr->arr.empty() &&
                          printerArr->arr[0].Find("name") != nullptr &&
                          printerArr->arr[0].Find("name")->ToConvertString() == printers[0].name;
        check("printers[0].name 正确", infoNameOk);

        // 兼容旧客户端的 printer 节点
        const JsonValue* compatNode = info.Find("printer");
        check("含兼容节点 printer", compatNode != nullptr && compatNode->type == JsonValue::OBJ);

        // ---- 404 ----
        body.clear();
        got = HttpRequest(L"GET", base + L"/api/unknown", body, status);
        check("未知端点返回 404", got && status == 404, "HTTP " + std::to_string(status));

        JsonValue errJson = JsonValue::Parse(body);
        check("404 响应含 error 字段", errJson.Find("error") != nullptr);

        // ---- OPTIONS 预检 ----
        body.clear();
        got = HttpRequest(L"OPTIONS", base + L"/api/test", body, status);
        check("OPTIONS 预检返回 200", got && status == 200, "HTTP " + std::to_string(status));

        server.Stop();
    }

    std::string all;
    {
        std::lock_guard<std::mutex> guard(logMutex);
        for (const auto& line : logs)
            all += line + "\n";
    }

    check("日志含 API 请求记录", all.find("API请求:") != std::string::npos);

    append("\r\n---- 服务日志（前 15 行）----\r\n");
    int shown = 0;
    {
        std::lock_guard<std::mutex> guard(logMutex);
        for (const auto& line : logs)
        {
            if (shown++ >= 15)
                break;
            append("%s\r\n", line.c_str());
        }
    }

    append("\r\n=========== 结果: %s (失败 %d 项) ===========\r\n",
           failed == 0 ? "全部通过" : "存在失败", failed);

    std::vector<char> out;
    const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    out.push_back((char)bom[0]);
    out.push_back((char)bom[1]);
    out.push_back((char)bom[2]);
    out.insert(out.end(), report.begin(), report.end());
    WriteAllBytes(reportPath, out);

    return failed;
}

// ===========================================================================
// 服务宿主端到端自测（不经 SCM，直接执行启动/停止主体）
// ===========================================================================

int RunServiceSelfTest(const std::wstring& reportPath)
{
    std::string report;
    int failed = 0;

    auto append = [&report](const char* fmt, ...) {
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
        va_end(args);
        report += buf;
    };

    auto check = [&](const char* name, bool pass, const std::string& detail = std::string()) {
        if (!pass)
            ++failed;
        append("[%s] %s%s%s\r\n", pass ? "PASS" : "FAIL", name,
               detail.empty() ? "" : " -> ", detail.c_str());
    };

    WSADATA wsa;
    ::WSAStartup(MAKEWORD(2, 2), &wsa);

    std::vector<PrinterConfig> configs = ConfigHelper::GetPrinterConfigs();
    std::string apiPortText = ConfigHelper::GetSetting("ApiPort", "9180");
    int apiPort = atoi(apiPortText.c_str());
    if (apiPort <= 0)
        apiPort = 9180;

    append("=========== 服务宿主端到端自测 ===========\r\n\r\n");
    append("配置打印机数量: %d\r\nHTTP API 端口: %d\r\n\r\n",
           (int)configs.size(), apiPort);

    for (size_t i = 0; i < configs.size(); ++i)
    {
        append("  [%d] %s -> 端口 %d (启用=%s)\r\n", (int)i, configs[i].name.c_str(),
               configs[i].port, configs[i].enabled ? "是" : "否");
    }
    append("\r\n");

    ServiceHost host;
    DWORD exitCode = host.StartStandalone();

    if (exitCode != 0)
    {
        // 启动门禁未通过或未配置打印机 → 服务必须拒绝启动（OnStart 的两处 return）
        append("启动被拒绝，退出码 = %lu\r\n\r\n", (unsigned long)exitCode);

        check("启动被阻止时退出码为 1066（与 C# 一致）", exitCode == 1066,
              std::to_string(exitCode));

        std::string log = Log::ReadTodayLog();
        const bool noPrinterBlocked = log.find("没有配置任何打印机，服务启动失败") != std::string::npos;
        check("记录了阻止原因", noPrinterBlocked, noPrinterBlocked ? "没有配置任何打印机" : "未找到");
    }
    else
    {
        check("服务启动成功（门禁通过且有打印机配置）", true);
        check("配置中存在启用的打印机", !configs.empty(),
              std::to_string(configs.size()) + " 台");

        for (size_t i = 0; i < configs.size(); ++i)
        {
            if (!configs[i].enabled)
                continue;

            size_t sent = 0;
            bool reachable = SendRawToPort("127.0.0.1", configs[i].port, 8, sent);
            check(("打印机端口可连接: " + configs[i].name).c_str(), reachable,
                  "端口 " + std::to_string(configs[i].port));
        }

        std::string body;
        int status = 0;
        std::wstring url = L"http://localhost:" + std::to_wstring(apiPort) + L"/api/test";
        bool got = HttpRequest(L"GET", url, body, status);
        check("HTTP API 可访问", got && status == 200, "HTTP " + std::to_string(status));
    }

    host.StopStandalone();

    // 停止后端口应已释放
    bool portReleased = true;
    for (size_t i = 0; i < configs.size(); ++i)
    {
        if (!configs[i].enabled)
            continue;

        size_t sent = 0;
        if (SendRawToPort("127.0.0.1", configs[i].port, 8, sent))
            portReleased = false;
    }
    check("停止后打印机端口已释放", portReleased);

    // -----------------------------------------------------------------------
    // 第二阶段：临时写入「2 台打印机」的配置，验证完整启动路径
    // -----------------------------------------------------------------------
    std::wstring configPath = Config::GetConfigPath();
    std::vector<char> savedConfig = ReadAllBytes(configPath);
    bool hadConfig = !savedConfig.empty();

    append("\r\n---- 第二阶段：临时配置 2 台打印机 ----\r\n");
    append("配置文件: %s（原文件 %s）\r\n\r\n",
           "PrintServer.exe.config", hadConfig ? "已备份" : "不存在");

    const int testPortA = 19101;
    const int testPortB = 19102;
    const int testApiPort = 19183;

    char testConfig[2048] = { 0 };
    _snprintf_s(testConfig, sizeof(testConfig), _TRUNCATE,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<configuration>\r\n"
        "  <appSettings>\r\n"
        "    <add key=\"PrinterCount\" value=\"2\"/>\r\n"
        "    <add key=\"Printer0_Name\" value=\"PS_SVC_TEST_A\"/>\r\n"
        "    <add key=\"Printer0_Port\" value=\"%d\"/>\r\n"
        "    <add key=\"Printer0_Enabled\" value=\"true\"/>\r\n"
        "    <add key=\"Printer1_Name\" value=\"PS_SVC_TEST_B\"/>\r\n"
        "    <add key=\"Printer1_Port\" value=\"%d\"/>\r\n"
        "    <add key=\"Printer1_Enabled\" value=\"true\"/>\r\n"
        "    <add key=\"ApiPort\" value=\"%d\"/>\r\n"
        "    <add key=\"LogRetentionDays\" value=\"15\"/>\r\n"
        "  </appSettings>\r\n"
        "</configuration>\r\n",
        testPortA, testPortB, testApiPort);

    {
        std::vector<char> bytes(testConfig, testConfig + strlen(testConfig));
        WriteAllBytes(configPath, bytes);
    }

    {
        ServiceHost host2;
        DWORD code2 = host2.StartStandalone();

        check("有打印机配置时服务启动成功", code2 == 0,
              "退出码 " + std::to_string(code2));

        size_t sentA = 0;
        check("打印机 A 端口已监听", SendRawToPort("127.0.0.1", testPortA, 8, sentA),
              "端口 " + std::to_string(testPortA));

        size_t sentB = 0;
        check("打印机 B 端口已监听", SendRawToPort("127.0.0.1", testPortB, 8, sentB),
              "端口 " + std::to_string(testPortB));

        std::string body;
        int status = 0;
        std::wstring url = L"http://localhost:" + std::to_wstring(testApiPort) + L"/api/test";
        bool got = HttpRequest(L"GET", url, body, status);
        check("HTTP API 已按新 ApiPort 启动", got && status == 200,
              "HTTP " + std::to_string(status));

        JsonValue json = JsonValue::Parse(body);
        const JsonValue* countNode = json.Find("printerCount");
        check("API 报告 printerCount == 2",
              countNode != nullptr && countNode->AsInt() == 2);

        std::string log = Log::ReadTodayLog();
        check("日志含「启动 2 台打印机」", log.find("启动 2 台打印机") != std::string::npos);

        host2.StopStandalone();

        size_t sentAfter = 0;
        check("停止后端口已释放（打印机 A）",
              !SendRawToPort("127.0.0.1", testPortA, 8, sentAfter));
    }

    // 还原原配置
    if (hadConfig)
    {
        WriteAllBytes(configPath, savedConfig);
        check("已还原原始配置文件", true);
    }
    else
    {
        ::DeleteFileW(configPath.c_str());
        check("已删除临时配置文件", true);
    }

    append("\r\n=========== 结果: %s (失败 %d 项) ===========\r\n",
           failed == 0 ? "全部通过" : "存在失败", failed);

    std::vector<char> out;
    const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    out.push_back((char)bom[0]);
    out.push_back((char)bom[1]);
    out.push_back((char)bom[2]);
    out.insert(out.end(), report.begin(), report.end());
    WriteAllBytes(reportPath, out);

    return failed;
}


}
#endif // _DEBUG
