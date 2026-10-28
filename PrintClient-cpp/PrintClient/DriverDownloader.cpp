#include "pch.h"
#include "DriverDownloader.h"

#include <string>
#include <vector>
#include <cstring>
#include <cctype>
#include <ctime>

#include "BuildConfig.h"
#include "DriverSigning.h"
#include "FileUtil.h"
#include "HttpClient.h"
#include "Json.h"
#include "Wmi.h"

#include "third_party/miniz/miniz.h"

namespace ps
{

namespace
{

std::wstring Utf8ToWideLocal(const std::string& text)
{
    return Utf8ToWide(text);
}

// ===========================================================================
// 驱动包清单支持（服务端 _printserver_driver.json）
// ===========================================================================

const char* const kManifestFile = "_printserver_driver.json";

inline unsigned RotateRight32(unsigned value, unsigned bits)
{
    return (value >> bits) | (value << (32 - bits));
}

const unsigned kSha256Table[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

// 自包含 SHA-256（避免引入 bcrypt 依赖，Win7/2008 同样可用）
class Sha256
{
public:
    Sha256()
    {
        m_state[0] = 0x6a09e667u; m_state[1] = 0xbb67ae85u;
        m_state[2] = 0x3c6ef372u; m_state[3] = 0xa54ff53au;
        m_state[4] = 0x510e527fu; m_state[5] = 0x9b05688cu;
        m_state[6] = 0x1f83d9abu; m_state[7] = 0x5be0cd19u;
        m_total = 0;
        m_bufferLength = 0;
    }

    void Update(const unsigned char* data, size_t length)
    {
        m_total += length;

        while (length > 0)
        {
            size_t space = 64 - m_bufferLength;
            size_t take = (length < space) ? length : space;

            memcpy(m_buffer + m_bufferLength, data, take);
            m_bufferLength += take;
            data += take;
            length -= take;

            if (m_bufferLength == 64)
            {
                Transform(m_buffer);
                m_bufferLength = 0;
            }
        }
    }

    std::string FinalHex()
    {
        unsigned long long bitLength = m_total * 8;

        unsigned char pad = 0x80;
        Update(&pad, 1);

        unsigned char zero = 0x00;
        while (m_bufferLength != 56)
            Update(&zero, 1);

        unsigned char lengthBytes[8] = { 0 };
        for (int i = 0; i < 8; ++i)
            lengthBytes[i] = (unsigned char)((bitLength >> (56 - i * 8)) & 0xFF);
        Update(lengthBytes, 8);

        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (int i = 0; i < 8; ++i)
        {
            for (int shift = 28; shift >= 0; shift -= 4)
            {
                unsigned nibble = (m_state[i] >> shift) & 0x0Fu;
                out.push_back(kHex[nibble]);
            }
        }
        return out;
    }

    // 与 FinalHex 相同的收尾逻辑，但返回 32 字节原始摘要（HMAC 需要）
    std::vector<unsigned char> FinalBytes()
    {
        unsigned long long bitLength = m_total * 8;

        unsigned char pad = 0x80;
        Update(&pad, 1);

        unsigned char zero = 0x00;
        while (m_bufferLength != 56)
            Update(&zero, 1);

        unsigned char lengthBytes[8] = { 0 };
        for (int i = 0; i < 8; ++i)
            lengthBytes[i] = (unsigned char)((bitLength >> (56 - i * 8)) & 0xFF);
        Update(lengthBytes, 8);

        std::vector<unsigned char> out(32);
        for (int i = 0; i < 8; ++i)
        {
            out[i * 4 + 0] = (unsigned char)((m_state[i] >> 24) & 0xFF);
            out[i * 4 + 1] = (unsigned char)((m_state[i] >> 16) & 0xFF);
            out[i * 4 + 2] = (unsigned char)((m_state[i] >> 8) & 0xFF);
            out[i * 4 + 3] = (unsigned char)(m_state[i] & 0xFF);
        }
        return out;
    }

private:
    void Transform(const unsigned char* block)
    {
        unsigned w[64];
        for (int i = 0; i < 16; ++i)
        {
            w[i] = ((unsigned)block[i * 4] << 24) | ((unsigned)block[i * 4 + 1] << 16) |
                   ((unsigned)block[i * 4 + 2] << 8) | (unsigned)block[i * 4 + 3];
        }
        for (int i = 16; i < 64; ++i)
        {
            unsigned s0 = RotateRight32(w[i - 15], 7) ^ RotateRight32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            unsigned s1 = RotateRight32(w[i - 2], 17) ^ RotateRight32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        unsigned a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
        unsigned e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];

        for (int i = 0; i < 64; ++i)
        {
            unsigned s1 = RotateRight32(e, 6) ^ RotateRight32(e, 11) ^ RotateRight32(e, 25);
            unsigned ch = (e & f) ^ ((~e) & g);
            unsigned temp1 = h + s1 + ch + kSha256Table[i] + w[i];
            unsigned s0 = RotateRight32(a, 2) ^ RotateRight32(a, 13) ^ RotateRight32(a, 22);
            unsigned maj = (a & b) ^ (a & c) ^ (b & c);
            unsigned temp2 = s0 + maj;

            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
        m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;
    }

    unsigned m_state[8];
    unsigned long long m_total;
    unsigned char m_buffer[64];
    size_t m_bufferLength;
};

std::string Sha256HexOfFile(const std::wstring& path)
{
    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(path, bytes))
        return std::string();

    Sha256 context;
    if (!bytes.empty())
        context.Update(bytes.data(), bytes.size());
    return context.FinalHex();
}

// 把清单里的相对路径（'/' 分隔）转为本地绝对路径
std::wstring ResolveManifestPath(const std::wstring& directory, const std::string& relative)
{
    std::wstring relativeWide = Utf8ToWide(relative);
    for (wchar_t& ch : relativeWide)
    {
        if (ch == L'/')
            ch = L'\\';
    }
    while (!relativeWide.empty() && relativeWide[0] == L'\\')
        relativeWide.erase(relativeWide.begin());

    return PathCombine(directory, relativeWide);
}

// ===========================================================================
// /api/driver 请求鉴权（HMAC-SHA256 + 时间戳）
// ===========================================================================

std::string Base64EncodeBytes(const std::vector<unsigned char>& data)
{
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 2 < data.size())
    {
        unsigned value = ((unsigned)data[i] << 16) | ((unsigned)data[i + 1] << 8) | data[i + 2];
        out.push_back(kTable[(value >> 18) & 0x3F]);
        out.push_back(kTable[(value >> 12) & 0x3F]);
        out.push_back(kTable[(value >> 6) & 0x3F]);
        out.push_back(kTable[value & 0x3F]);
        i += 3;
    }

    if (i + 1 == data.size())
    {
        unsigned value = (unsigned)data[i] << 16;
        out.push_back(kTable[(value >> 18) & 0x3F]);
        out.push_back(kTable[(value >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    }
    else if (i + 2 == data.size())
    {
        unsigned value = ((unsigned)data[i] << 16) | ((unsigned)data[i + 1] << 8);
        out.push_back(kTable[(value >> 18) & 0x3F]);
        out.push_back(kTable[(value >> 12) & 0x3F]);
        out.push_back(kTable[(value >> 6) & 0x3F]);
        out.push_back('=');
    }

    return out;
}

std::vector<unsigned char> HmacSha256(const std::vector<unsigned char>& key,
                                      const std::string& message)
{
    const size_t blockSize = 64;

    std::vector<unsigned char> normalized = key;
    if (normalized.size() > blockSize)
    {
        Sha256 context;
        context.Update(normalized.data(), normalized.size());
        normalized = context.FinalBytes();
    }
    normalized.resize(blockSize, 0);

    std::vector<unsigned char> inner(blockSize);
    std::vector<unsigned char> outer(blockSize);
    for (size_t i = 0; i < blockSize; ++i)
    {
        inner[i] = (unsigned char)(normalized[i] ^ 0x36);
        outer[i] = (unsigned char)(normalized[i] ^ 0x5C);
    }

    Sha256 innerContext;
    innerContext.Update(inner.data(), inner.size());
    if (!message.empty())
        innerContext.Update((const unsigned char*)message.data(), message.size());
    std::vector<unsigned char> innerHash = innerContext.FinalBytes();

    Sha256 outerContext;
    outerContext.Update(outer.data(), outer.size());
    outerContext.Update(innerHash.data(), innerHash.size());
    return outerContext.FinalBytes();
}

// 生成鉴权头：签名对象为 "<时间戳>|<请求目标>"，与服务端校验逻辑一致
Http::HeaderList BuildAuthHeaders(const std::string& target)
{
    Http::HeaderList headers;

    long long now = (long long)::time(nullptr);
    std::string timestamp = std::to_string(now);

    std::string secret = BuildConfig::ApiAuthSecret;
    std::vector<unsigned char> key(secret.begin(), secret.end());
    std::vector<unsigned char> mac = HmacSha256(key, timestamp + "|" + target);

    headers.push_back(std::make_pair("X-PS-Time", timestamp));
    headers.push_back(std::make_pair("X-PS-Auth", Base64EncodeBytes(mac)));
    return headers;
}

// 对应 C# 里 "try { info.X = data[...]; } catch { }" 的严格字符串赋值：
// 只有 JSON 字符串才会写入，其它类型（int/bool/对象）会在 C# 里抛出
// InvalidCastException 并被吞掉，字段保持原值。
void SetStrictString(const JsonValue* value, std::string& target)
{
    if (value != nullptr && value->type == JsonValue::STR)
        target = value->s;
}

// 对应 Convert.ToString(item["X"])：任意类型都接受，null → ""
void SetConvertString(const JsonValue* value, std::string& target)
{
    if (value != nullptr)
        target = value->ToConvertString();
}

// 对应 Convert.ToInt32(item["X"])：解析失败/类型不符时 C# 抛异常并被吞掉
void SetConvertInt(const JsonValue* value, int& target)
{
    if (value == nullptr)
        return;

    switch (value->type)
    {
    case JsonValue::INT:
        target = (int)value->i;
        break;
    case JsonValue::DBL:
        target = (int)value->d;
        break;
    case JsonValue::BOOL:
        target = value->b ? 1 : 0;
        break;
    case JsonValue::STR:
        target = ::atoi(value->s.c_str());
        break;
    case JsonValue::NUL:
        target = 0;     // Convert.ToInt32(null) == 0
        break;
    default:
        break;
    }
}

// 对应 Convert.ToBoolean(item["X"])
void SetConvertBool(const JsonValue* value, bool& target)
{
    if (value == nullptr)
        return;

    switch (value->type)
    {
    case JsonValue::BOOL:
        target = value->b;
        break;
    case JsonValue::INT:
        target = (value->i != 0);
        break;
    case JsonValue::DBL:
        target = (value->d != 0.0);
        break;
    case JsonValue::STR:
        if (_stricmp(value->s.c_str(), "true") == 0)
            target = true;
        else if (_stricmp(value->s.c_str(), "false") == 0)
            target = false;
        break;
    case JsonValue::NUL:
        target = false;     // Convert.ToBoolean(null) == false
        break;
    default:
        break;
    }
}

// zip 条目名 → 宽字符：优先按 UTF-8（bit 11 或合法 UTF-8），否则按系统 ANSI
std::wstring DecodeZipEntryName(const char* name, mz_uint16 bitFlag)
{
    std::string raw = name;

    if ((bitFlag & 0x800) != 0)
        return Utf8ToWide(raw);

    // miniz 写入时对非 ASCII 名称会置 UTF-8 标志；这里再兜底判断一次合法性
    int wideLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.c_str(),
                                           (int)raw.size(), nullptr, 0);
    if (wideLength > 0)
        return Utf8ToWide(raw);

    int ansiLength = ::MultiByteToWideChar(CP_ACP, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
    if (ansiLength <= 0)
        return Utf8ToWide(raw);

    std::wstring wide((size_t)ansiLength, L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, raw.c_str(), (int)raw.size(), &wide[0], ansiLength);
    return wide;
}

} // namespace

DriverDownloader::DriverDownloader(const std::string& serverHost, int apiPort)
{
    // 对应 string.Format("http://{0}:{1}", serverHost, apiPort)
    m_serverBaseUrl = "http://" + serverHost + ":" + std::to_string(apiPort);
}

bool DriverDownloader::TestConnection()
{
    try
    {
        Http::Response response;
        std::string error;

        std::wstring url = Utf8ToWide(m_serverBaseUrl + "/api/test");
        if (!Http::Get(url, 5000, response, error))
            return false;

        return Http::IsSuccess(response);
    }
    catch (...)
    {
        return false;
    }
}

bool DriverDownloader::GetPrinterInfo(ServerPrinterInfo& info, std::string& error)
{
    try
    {
        Http::Response response;
        std::string httpError;

        std::wstring url = Utf8ToWide(m_serverBaseUrl + "/api/info");
        if (!Http::Get(url, 10000, response, httpError))
        {
            error = "获取打印机信息失败: " + httpError;
            return false;
        }

        if (!Http::IsSuccess(response))
        {
            error = "获取打印机信息失败: " + Http::EnsureSuccessMessage(response);
            return false;
        }

        std::string parseError;
        JsonValue data = JsonValue::Parse(response.body, &parseError);
        if (!parseError.empty() || data.type != JsonValue::OBJ)
        {
            error = "获取打印机信息失败: " + (parseError.empty() ? std::string("JSON 数据格式无效") : parseError);
            return false;
        }

        const JsonValue* server = data.Find("server");
        if (server != nullptr && server->type == JsonValue::OBJ)
        {
            SetStrictString(server->Find("hostname"), info.hostname);
            SetStrictString(server->Find("version"), info.version);
            SetConvertInt(server->Find("apiPort"), info.apiPort);
            SetConvertInt(server->Find("printerCount"), info.printerCount);
        }

        // 兼容旧版API：顶层printer字段
        const JsonValue* printer = data.Find("printer");
        if (printer != nullptr && printer->type == JsonValue::OBJ)
        {
            SetStrictString(printer->Find("name"), info.printerName);
            SetConvertInt(printer->Find("tcpPort"), info.tcpPort);
            SetStrictString(printer->Find("driverName"), info.driverName);
            SetStrictString(printer->Find("driverVersion"), info.driverVersion);
            SetStrictString(printer->Find("driverDate"), info.driverDate);
            SetStrictString(printer->Find("manufacturer"), info.manufacturer);
            SetConvertBool(printer->Find("hasDriver"), info.hasDriver);
            SetConvertInt(printer->Find("driverFileCount"), info.driverFileCount);
            SetStrictString(printer->Find("driverSizeFormatted"), info.driverSize);
        }

        // 新版API：printers数组
        const JsonValue* printers = data.Find("printers");
        if (printers != nullptr && printers->type == JsonValue::ARR)
        {
            for (const JsonValue& entry : printers->arr)
            {
                if (entry.type != JsonValue::OBJ)
                    continue;

                PrinterItemInfo item;
                SetConvertInt(entry.Find("index"), item.index);
                SetConvertString(entry.Find("name"), item.name);
                SetConvertInt(entry.Find("port"), item.port);
                SetConvertString(entry.Find("driverName"), item.driverName);
                SetConvertString(entry.Find("driverVersion"), item.driverVersion);
                SetConvertString(entry.Find("driverDate"), item.driverDate);
                SetConvertString(entry.Find("manufacturer"), item.manufacturer);
                SetConvertBool(entry.Find("hasDriver"), item.hasDriver);
                SetConvertInt(entry.Find("driverFileCount"), item.driverFileCount);
                SetConvertString(entry.Find("driverSizeFormatted"), item.driverSize);

                info.printers.push_back(item);
            }
        }

        // 如果printers为空但printer存在，用printer构建列表
        if (info.printers.empty() && !info.printerName.empty())
        {
            PrinterItemInfo item;
            item.index = 0;
            item.name = info.printerName;
            item.port = info.tcpPort;
            item.driverName = info.driverName;
            item.driverVersion = info.driverVersion;
            item.driverDate = info.driverDate;
            item.manufacturer = info.manufacturer;
            item.hasDriver = info.hasDriver;
            item.driverFileCount = info.driverFileCount;
            item.driverSize = info.driverSize;

            info.printers.push_back(item);
            info.printerCount = 1;
        }

        return true;
    }
    catch (const std::exception& ex)
    {
        error = std::string("获取打印机信息失败: ") + ex.what();
        return false;
    }
    catch (...)
    {
        error = "获取打印机信息失败: 未知错误";
        return false;
    }
}

bool DriverDownloader::DownloadDriver(const std::string& saveDirectory, int printerIndex,
                                      const std::function<void(int)>& progress,
                                      std::string& zipPath, std::string& error)
{
    try
    {
        std::wstring wideDirectory = Utf8ToWide(saveDirectory);
        if (!DirectoryExists(wideDirectory))
            EnsureDirectory(wideDirectory);

        zipPath = WideToUtf8(PathCombine(wideDirectory, L"printer_driver.zip"));

        std::string target = "/api/driver";
        if (printerIndex > 0)
            target += "?id=" + std::to_string(printerIndex);

        std::string urlText = m_serverBaseUrl + target;

        HANDLE file = ::CreateFileW(Utf8ToWide(zipPath).c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = "下载驱动失败: 无法创建文件";
            return false;
        }

        bool writeFailed = false;

        // 对应 8192 字节缓冲区 + ReadAsync/WriteAsync 循环
        auto onChunk = [&](const char* data, size_t size, long long received, long long total) -> bool {
            if (size > 0)
            {
                DWORD written = 0;
                if (!::WriteFile(file, data, (DWORD)size, &written, nullptr) || written != (DWORD)size)
                {
                    writeFailed = true;
                    return false;
                }
            }

            if (progress && total > 0)
            {
                int percent = (int)(received * 100 / total);
                progress(percent);
            }

            return true;
        };

        Http::Response response;
        std::string httpError;

        // /api/driver 需要 HMAC 时间戳鉴权（服务端会校验，失败返回 403）
        Http::HeaderList authHeaders = BuildAuthHeaders(target);

        bool ok = Http::Download(Utf8ToWide(urlText), 5 * 60 * 1000, onChunk, response, httpError,
                                 authHeaders);

        ::CloseHandle(file);

        if (!ok)
        {
            error = "下载驱动失败: " + (writeFailed ? std::string("文件写入失败") : httpError);
            return false;
        }

        if (!Http::IsSuccess(response))
        {
            error = "下载驱动失败: " + Http::EnsureSuccessMessage(response);
            return false;
        }

        return true;
    }
    catch (const std::exception& ex)
    {
        error = std::string("下载驱动失败: ") + ex.what();
        return false;
    }
    catch (...)
    {
        error = "下载驱动失败: 未知错误";
        return false;
    }
}

bool DriverDownloader::ExtractDriver(const std::string& zipPath, const std::string& extractDir,
                                     std::string& error)
{
    try
    {
        std::wstring wideExtractDir = Utf8ToWide(extractDir);
        std::wstring wideZipPath = Utf8ToWide(zipPath);

        // 对应 Directory.Delete(extractDir, true)
        if (DirectoryExists(wideExtractDir))
            RemoveDirectoryTree(wideExtractDir);

        EnsureDirectory(wideExtractDir);

        std::vector<unsigned char> archive;
        if (!ReadFileBytes(wideZipPath, archive))
        {
            error = "解压驱动失败: 无法读取压缩包";
            return false;
        }

        mz_zip_archive zip;
        memset(&zip, 0, sizeof(zip));

        if (!mz_zip_reader_init_mem(&zip, archive.data(), archive.size(), 0))
        {
            error = "解压驱动失败: 压缩包格式无效";
            return false;
        }

        bool failed = false;
        std::string failureText;

        mz_uint fileCount = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < fileCount && !failed; ++i)
        {
            mz_zip_archive_file_stat stat;
            memset(&stat, 0, sizeof(stat));

            if (!mz_zip_reader_file_stat(&zip, i, &stat))
                continue;

            std::wstring name = DecodeZipEntryName(stat.m_filename, stat.m_bit_flag);

            // zip 内部统一用 '/' 分隔
            for (wchar_t& ch : name)
            {
                if (ch == L'/')
                    ch = L'\\';
            }

            while (!name.empty() && (name[0] == L'\\' || name[0] == L'/'))
                name.erase(name.begin());

            if (name.empty())
                continue;

            std::wstring target = PathCombine(wideExtractDir, name);

            if (mz_zip_reader_is_file_a_directory(&zip, i) ||
                (!name.empty() && name.back() == L'\\'))
            {
                CreateDirectoryTree(target);
                continue;
            }

            size_t slash = target.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
                CreateDirectoryTree(target.substr(0, slash));

            std::vector<unsigned char> content((size_t)stat.m_uncomp_size);
            if (stat.m_uncomp_size > 0)
            {
                if (!mz_zip_reader_extract_to_mem(&zip, i, content.data(), content.size(), 0))
                {
                    failed = true;
                    failureText = "数据校验失败";
                    break;
                }
            }

            if (!WriteFileBytes(target, content))
            {
                failed = true;
                failureText = "写入文件失败";
                break;
            }
        }

        mz_zip_reader_end(&zip);

        if (failed)
        {
            error = "解压驱动失败: " + failureText;
            return false;
        }

        // 解压后校验：
        //   1) 清单数字签名 —— 防止中间人替换整包（改包的人改不了签名）
        //   2) 逐文件 SHA256 —— 防止截断、损坏、内容被替换
        DriverManifest manifest;
        LoadManifest(extractDir, manifest);
        if (manifest.present)
        {
            if (!manifest.signatureValid)
            {
                error = "解压驱动失败: 驱动包数字签名校验失败（包可能被篡改，或服务端版本过旧）";
                return false;
            }

            if (!manifest.files.empty())
            {
                std::string verifyError;
                if (!VerifyManifestFiles(extractDir, manifest, verifyError))
                {
                    error = "解压驱动失败: " + verifyError;
                    return false;
                }
            }
        }

        return true;
    }
    catch (const std::exception& ex)
    {
        error = std::string("解压驱动失败: ") + ex.what();
        return false;
    }
    catch (...)
    {
        error = "解压驱动失败: 未知错误";
        return false;
    }
}

// ===========================================================================
// 驱动包清单
// ===========================================================================

const char* DriverDownloader::ManifestFileName()
{
    return kManifestFile;
}

bool DriverDownloader::LoadManifest(const std::string& driverDirectory, DriverManifest& manifest)
{
    manifest = DriverManifest();

    try
    {
        std::wstring directory = Utf8ToWide(driverDirectory);
        std::wstring manifestPath = PathCombine(directory, Utf8ToWide(kManifestFile));

        std::vector<unsigned char> bytes;
        if (!ReadFileBytes(manifestPath, bytes) || bytes.empty())
            return true;   // 兼容旧版服务端：无清单不算失败

        std::string text((const char*)bytes.data(), bytes.size());
        if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
            (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        {
            text.erase(0, 3);
        }

        std::string parseError;
        JsonValue root = JsonValue::Parse(text, &parseError);
        if (!parseError.empty() || root.type != JsonValue::OBJ)
        {
            manifest.present = true;   // 清单存在但不可解析
            return true;
        }

        manifest.present = true;

        if (const JsonValue* value = root.Find("driverName"))
            manifest.driverName = value->AsString();
        if (const JsonValue* value = root.Find("inf"))
            manifest.inf = value->AsString();
        if (const JsonValue* value = root.Find("architecture"))
            manifest.architecture = value->AsString();
        if (const JsonValue* value = root.Find("source"))
            manifest.source = value->AsString();
        if (const JsonValue* value = root.Find("catalogFile"))
            manifest.catalogFile = value->AsString();
        if (const JsonValue* value = root.Find("complete"))
            manifest.complete = value->AsBool(true);

        if (const JsonValue* value = root.Find("missing"))
        {
            if (value->type == JsonValue::ARR)
            {
                for (const JsonValue& item : value->arr)
                    manifest.missing.push_back(item.AsString());
            }
        }

        if (const JsonValue* value = root.Find("infModels"))
        {
            if (value->type == JsonValue::ARR)
            {
                for (const JsonValue& item : value->arr)
                    manifest.infModels.push_back(item.AsString());
            }
        }

        if (const JsonValue* value = root.Find("files"))
        {
            if (value->type == JsonValue::ARR)
            {
                for (const JsonValue& item : value->arr)
                {
                    if (item.type != JsonValue::OBJ)
                        continue;

                    ManifestFileEntry entry;
                    if (const JsonValue* path = item.Find("path"))
                        entry.path = path->AsString();
                    if (const JsonValue* size = item.Find("size"))
                        entry.size = size->AsInt(0);
                    if (const JsonValue* sha = item.Find("sha256"))
                        entry.sha256 = sha->AsString();

                    if (!entry.path.empty())
                        manifest.files.push_back(entry);
                }
            }
        }

        if (const JsonValue* value = root.Find("signature"))
            manifest.signature = value->AsString();
        manifest.signaturePresent = !manifest.signature.empty();

        // 验签：载荷必须与服务端 DriverSigning::BuildPayload 逐字节一致
        {
            std::vector<std::string> paths;
            std::vector<std::string> hashes;
            paths.reserve(manifest.files.size());
            hashes.reserve(manifest.files.size());

            for (const auto& entry : manifest.files)
            {
                paths.push_back(entry.path);
                hashes.push_back(entry.sha256);
            }

            std::string payload = DriverSigning::BuildPayload(manifest.driverName,
                                                             manifest.inf,
                                                             manifest.architecture,
                                                             manifest.catalogFile,
                                                             paths, hashes);
            manifest.signatureValid = manifest.signaturePresent &&
                                      DriverSigning::Verify(payload, manifest.signature);
        }

        return true;
    }
    catch (...)
    {
        return true;   // 读取清单失败不应阻断安装
    }
}

bool DriverDownloader::VerifyManifestFiles(const std::string& driverDirectory,
                                           const DriverManifest& manifest, std::string& error)
{
    if (!manifest.present || manifest.files.empty())
        return true;

    std::wstring directory = Utf8ToWide(driverDirectory);

    for (const auto& entry : manifest.files)
    {
        if (entry.sha256.empty())
            continue;

        std::wstring path = ResolveManifestPath(directory, entry.path);
        if (!FileExists(path))
        {
            error = "驱动包缺少清单声明的文件: " + entry.path;
            return false;
        }

        std::string expected = entry.sha256;
        for (char& ch : expected)
            ch = (char)::tolower((unsigned char)ch);

        std::string actual = Sha256HexOfFile(path);
        if (actual.empty())
        {
            error = "驱动文件读取失败: " + entry.path;
            return false;
        }

        if (actual != expected)
        {
            error = "驱动文件校验失败(内容不一致): " + entry.path;
            return false;
        }
    }

    return true;
}

} // namespace ps
